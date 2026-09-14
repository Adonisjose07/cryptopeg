const { expect } = require("chai");
const { ethers } = require("hardhat");

describe("CryptoPegVault (V1) Unit & Security Tests", function () {
  let usdt, vault;
  let owner, validator, alice, bob, treasury, attacker;

  const DEPOSIT_FEE_BPS = 50;  // 0.50%
  const WITHDRAW_FEE_BPS = 50; // 0.50%

  beforeEach(async function () {
    [owner, validator, alice, bob, treasury, attacker] = await ethers.getSigners();

    const MockUSDT = await ethers.getContractFactory("MockUSDT");
    usdt = await MockUSDT.deploy();
    await usdt.waitForDeployment();

    const CryptoPegVault = await ethers.getContractFactory("CryptoPegVault");
    vault = await CryptoPegVault.deploy(
      await usdt.getAddress(),
      validator.address,
      DEPOSIT_FEE_BPS,
      WITHDRAW_FEE_BPS,
      owner.address
    );
    await vault.waitForDeployment();

    // Fondear a Alice
    await usdt.mint(alice.address, ethers.parseUnits("10000", 6));
    await usdt.connect(alice).approve(await vault.getAddress(), ethers.MaxUint256);
  });

  describe("Constructor & Inicialización", function () {
    it("debe revertir si USDT address es cero", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVault");
      await expect(
        Vault.deploy(ethers.ZeroAddress, validator.address, 50, 50, owner.address)
      ).to.be.revertedWith("USDT address cannot be 0");
    });

    it("debe revertir si validatorSigner es cero", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVault");
      await expect(
        Vault.deploy(await usdt.getAddress(), ethers.ZeroAddress, 50, 50, owner.address)
      ).to.be.revertedWith("Validator signer cannot be 0");
    });

    it("debe revertir si comisiones superan 1000 bps (10%)", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVault");
      await expect(
        Vault.deploy(await usdt.getAddress(), validator.address, 1001, 50, owner.address)
      ).to.be.revertedWith("Fee cannot exceed 10%");
      await expect(
        Vault.deploy(await usdt.getAddress(), validator.address, 50, 1001, owner.address)
      ).to.be.revertedWith("Fee cannot exceed 10%");
    });

    it("debe inicializar variables de estado correctamente", async function () {
      expect(await vault.usdtToken()).to.equal(await usdt.getAddress());
      expect(await vault.validatorSigner()).to.equal(validator.address);
      expect(await vault.depositFeeBps()).to.equal(DEPOSIT_FEE_BPS);
      expect(await vault.withdrawFeeBps()).to.equal(WITHDRAW_FEE_BPS);
      expect(await vault.accumulatedFees()).to.equal(0);
      expect(await vault.owner()).to.equal(owner.address);
    });
  });

  describe("Depósitos (deposit)", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    it("debe revertir si monto es 0", async function () {
      await expect(
        vault.connect(alice).deposit(0, fakeView, fakeSpend)
      ).to.be.revertedWith("Deposit amount must be > 0");
    });

    it("debe revertir si stealth view key es 0", async function () {
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("100", 6), ethers.ZeroHash, fakeSpend)
      ).to.be.revertedWith("Invalid stealth view key");
    });

    it("debe revertir si stealth spend key es 0", async function () {
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("100", 6), fakeView, ethers.ZeroHash)
      ).to.be.revertedWith("Invalid stealth spend key");
    });

    it("debe procesar depósito válido, deducir comisión y emitir DepositInitiated", async function () {
      const gross = ethers.parseUnits("1000", 6);
      const expectedFee = (gross * BigInt(DEPOSIT_FEE_BPS)) / 10000n; // 5 USDT
      const expectedNet = gross - expectedFee;                         // 995 USDT

      await expect(vault.connect(alice).deposit(gross, fakeView, fakeSpend))
        .to.emit(vault, "DepositInitiated")
        .withArgs(alice.address, gross, expectedNet, expectedFee, fakeView, fakeSpend, (v) => true);

      expect(await vault.accumulatedFees()).to.equal(expectedFee);
      expect(await vault.getCollateralBalance()).to.equal(gross);
      expect(await vault.getCirculatingBacking()).to.equal(expectedNet);
    });

    it("debe revertir si el contrato está pausado", async function () {
      await vault.connect(owner).pause();
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("100", 6), fakeView, fakeSpend)
      ).to.be.revertedWithCustomError(vault, "EnforcedPause");
    });
  });

  describe("Retiros (withdraw)", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    beforeEach(async function () {
      // Alice deposita 1,000 USDT para fondear la bóveda
      await vault.connect(alice).deposit(ethers.parseUnits("1000", 6), fakeView, fakeSpend);
    });

    async function generateSignature(orderId, recipient, amount, signer, chainIdOverride, vaultOverride) {
      const cId = chainIdOverride !== undefined ? chainIdOverride : (await ethers.provider.getNetwork()).chainId;
      const vAddr = vaultOverride !== undefined ? vaultOverride : await vault.getAddress();
      const messageHash = ethers.solidityPackedKeccak256(
        ["bytes32", "address", "uint256", "uint256", "address"],
        [orderId, recipient, amount, cId, vAddr]
      );
      return await signer.signMessage(ethers.getBytes(messageHash));
    }

    it("debe ejecutar retiro válido y transferir fondos al destinatario", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("200", 6);
      const sig = await generateSignature(orderId, bob.address, amount, validator);

      const bobBefore = await usdt.balanceOf(bob.address);
      await expect(vault.connect(bob).withdraw(orderId, bob.address, amount, sig))
        .to.emit(vault, "WithdrawalExecuted")
        .withArgs(orderId, bob.address, amount, (v) => true);

      const bobAfter = await usdt.balanceOf(bob.address);
      expect(bobAfter - bobBefore).to.equal(amount);
      expect(await vault.executedWithdrawals(orderId)).to.be.true;
    });

    it("debe prevenir doble retiro (Replay Attack con mismo orderId)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await generateSignature(orderId, bob.address, amount, validator);

      await vault.connect(bob).withdraw(orderId, bob.address, amount, sig);
      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, sig)
      ).to.be.revertedWith("Withdrawal order already executed");
    });

    it("debe rechazar retiro con destinatario dirección cero", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await generateSignature(orderId, ethers.ZeroAddress, amount, validator);

      await expect(
        vault.connect(bob).withdraw(orderId, ethers.ZeroAddress, amount, sig)
      ).to.be.revertedWith("Invalid recipient address");
    });

    it("debe rechazar retiro con monto cero", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const sig = await generateSignature(orderId, bob.address, 0, validator);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, 0, sig)
      ).to.be.revertedWith("Withdrawal amount must be > 0");
    });

    it("debe rechazar firma de un atacante no autorizado", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const evilSig = await generateSignature(orderId, bob.address, amount, attacker);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, evilSig)
      ).to.be.revertedWith("Invalid validator signature for withdrawal");
    });

    it("debe rechazar firma generada para otra chainId (Cross-Chain Replay)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const wrongChainSig = await generateSignature(orderId, bob.address, amount, validator, 999999);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, wrongChainSig)
      ).to.be.revertedWith("Invalid validator signature for withdrawal");
    });

    it("debe rechazar firma generada para otro contrato de bóveda (Cross-Contract Replay)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const wrongVaultSig = await generateSignature(orderId, bob.address, amount, validator, undefined, attacker.address);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, wrongVaultSig)
      ).to.be.revertedWith("Invalid validator signature for withdrawal");
    });

    it("debe revertir si el contrato está pausado", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await generateSignature(orderId, bob.address, amount, validator);

      await vault.connect(owner).pause();
      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, sig)
      ).to.be.revertedWithCustomError(vault, "EnforcedPause");
    });
  });

  describe("Administración y Tesorería", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    beforeEach(async function () {
      // Depositar 10,000 USDT para generar comisiones de 50 USDT
      await vault.connect(alice).deposit(ethers.parseUnits("10000", 6), fakeView, fakeSpend);
    });

    it("debe permitir al Owner reclamar comisiones de tesorería", async function () {
      const accFees = await vault.accumulatedFees();
      expect(accFees).to.be.gt(0);

      const claimAmount = ethers.parseUnits("20", 6);
      await expect(vault.connect(owner).claimTreasuryFees(treasury.address, claimAmount))
        .to.emit(vault, "TreasuryFeesClaimed")
        .withArgs(treasury.address, claimAmount, accFees - claimAmount, (v) => true);

      expect(await usdt.balanceOf(treasury.address)).to.equal(claimAmount);
      expect(await vault.accumulatedFees()).to.equal(accFees - claimAmount);
    });

    it("debe rechazar reclamo de comisiones por no-propietario", async function () {
      await expect(
        vault.connect(attacker).claimTreasuryFees(attacker.address, ethers.parseUnits("10", 6))
      ).to.be.revertedWithCustomError(vault, "OwnableUnauthorizedAccount");
    });

    it("debe rechazar reclamo con destino dirección cero", async function () {
      await expect(
        vault.connect(owner).claimTreasuryFees(ethers.ZeroAddress, ethers.parseUnits("10", 6))
      ).to.be.revertedWith("Invalid treasury destination address");
    });

    it("debe rechazar reclamo con monto cero", async function () {
      await expect(
        vault.connect(owner).claimTreasuryFees(treasury.address, 0)
      ).to.be.revertedWith("Claim amount must be > 0");
    });

    it("debe rechazar reclamo por monto superior a comisiones acumuladas", async function () {
      const accFees = await vault.accumulatedFees();
      await expect(
        vault.connect(owner).claimTreasuryFees(treasury.address, accFees + 1n)
      ).to.be.revertedWith("Amount exceeds available fee pool reserves");
    });

    it("debe permitir al Owner actualizar el validatorSigner", async function () {
      const newValidator = bob.address;
      await expect(vault.connect(owner).setValidatorSigner(newValidator))
        .to.emit(vault, "ValidatorSignerUpdated")
        .withArgs(validator.address, newValidator);

      expect(await vault.validatorSigner()).to.equal(newValidator);
    });

    it("debe rechazar actualización de validatorSigner a dirección cero", async function () {
      await expect(
        vault.connect(owner).setValidatorSigner(ethers.ZeroAddress)
      ).to.be.revertedWith("New signer cannot be address 0");
    });

    it("debe permitir al Owner actualizar comisiones de depósito y retiro", async function () {
      await expect(vault.connect(owner).setFees(100, 200))
        .to.emit(vault, "FeeRatesUpdated")
        .withArgs(100, 200);

      expect(await vault.depositFeeBps()).to.equal(100);
      expect(await vault.withdrawFeeBps()).to.equal(200);
    });

    it("debe rechazar comisiones superiores al 10%", async function () {
      await expect(vault.connect(owner).setFees(1001, 100)).to.be.revertedWith("Fee cannot exceed 10%");
      await expect(vault.connect(owner).setFees(100, 1001)).to.be.revertedWith("Fee cannot exceed 10%");
    });

    it("debe permitir pausar y despausar solo al Owner", async function () {
      await expect(vault.connect(attacker).pause()).to.be.revertedWithCustomError(vault, "OwnableUnauthorizedAccount");
      await vault.connect(owner).pause();
      expect(await vault.paused()).to.be.true;

      await expect(vault.connect(attacker).unpause()).to.be.revertedWithCustomError(vault, "OwnableUnauthorizedAccount");
      await vault.connect(owner).unpause();
      expect(await vault.paused()).to.be.false;
    });

    it("getCirculatingBacking debe calcular el respaldo y retornar 0 si total balance <= accumulatedFees (QA-GAP-01)", async function () {
      expect(await vault.getCirculatingBacking()).to.be.gt(0);

      // Desplegar una bóveda sin depósitos (balance 0 <= fees 0) para activar la rama de retorno 0
      const VaultFactory = await ethers.getContractFactory("CryptoPegVault");
      const emptyVault = await VaultFactory.deploy(await usdt.getAddress(), validator.address, 50, 50, owner.address);
      expect(await emptyVault.getCirculatingBacking()).to.equal(0);
    });
  });
});
