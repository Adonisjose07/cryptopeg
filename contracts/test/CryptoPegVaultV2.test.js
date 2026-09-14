const { expect } = require("chai");
const { ethers } = require("hardhat");

describe("CryptoPegVaultV2 (EIP-712 & Dual Fallback) Unit & Security Tests", function () {
  let usdt, vault;
  let owner, validator, alice, bob, treasury, attacker;

  const DEPOSIT_FEE_BPS = 50;  // 0.50%
  const WITHDRAW_FEE_BPS = 50; // 0.50%

  beforeEach(async function () {
    [owner, validator, alice, bob, treasury, attacker] = await ethers.getSigners();

    const MockUSDT = await ethers.getContractFactory("MockUSDT");
    usdt = await MockUSDT.deploy();
    await usdt.waitForDeployment();

    const CryptoPegVaultV2 = await ethers.getContractFactory("CryptoPegVaultV2");
    vault = await CryptoPegVaultV2.deploy(
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

  describe("Constructor & Inicialización V2", function () {
    it("debe revertir si USDT address es cero", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVaultV2");
      await expect(
        Vault.deploy(ethers.ZeroAddress, validator.address, 50, 50, owner.address)
      ).to.be.revertedWith("USDT address cannot be 0");
    });

    it("debe revertir si validatorSigner es cero", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVaultV2");
      await expect(
        Vault.deploy(await usdt.getAddress(), ethers.ZeroAddress, 50, 50, owner.address)
      ).to.be.revertedWith("Validator signer cannot be 0");
    });

    it("debe revertir si comisiones superan 1000 bps", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVaultV2");
      await expect(
        Vault.deploy(await usdt.getAddress(), validator.address, 1001, 50, owner.address)
      ).to.be.revertedWith("Fee cannot exceed 10%");
      await expect(
        Vault.deploy(await usdt.getAddress(), validator.address, 50, 1001, owner.address)
      ).to.be.revertedWith("Fee cannot exceed 10%");
    });

    it("debe inicializar correctamente las constantes y TypeHash", async function () {
      expect(await vault.usdtToken()).to.equal(await usdt.getAddress());
      expect(await vault.validatorSigner()).to.equal(validator.address);
      expect(await vault.depositFeeBps()).to.equal(DEPOSIT_FEE_BPS);
      expect(await vault.withdrawFeeBps()).to.equal(WITHDRAW_FEE_BPS);
      expect(await vault.WITHDRAWAL_TYPEHASH()).to.equal(
        ethers.keccak256(ethers.toUtf8Bytes("Withdrawal(bytes32 orderId,address recipient,uint256 amount,uint256 fee)"))
      );
    });
  });

  describe("Depósitos V2", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    it("debe rechazar depósito con monto cero", async function () {
      await expect(
        vault.connect(alice).deposit(0, fakeView, fakeSpend)
      ).to.be.revertedWith("Deposit amount must be > 0");
    });

    it("debe rechazar claves furtivas nulas", async function () {
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("10", 6), ethers.ZeroHash, fakeSpend)
      ).to.be.revertedWith("Invalid stealth view key");
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("10", 6), fakeView, ethers.ZeroHash)
      ).to.be.revertedWith("Invalid stealth spend key");
    });

    it("debe procesar depósito válido con deducción exacta", async function () {
      const gross = ethers.parseUnits("1000", 6);
      const expectedFee = (gross * 50n) / 10000n; // 5 USDT
      const expectedNet = gross - expectedFee;      // 995 USDT

      await expect(vault.connect(alice).deposit(gross, fakeView, fakeSpend))
        .to.emit(vault, "DepositInitiated")
        .withArgs(alice.address, gross, expectedNet, expectedFee, fakeView, fakeSpend, (v) => true);

      expect(await vault.accumulatedFees()).to.equal(expectedFee);
      expect(await vault.getCollateralBalance()).to.equal(gross);
      expect(await vault.getCirculatingBacking()).to.equal(expectedNet);
    });

    it("debe revertir depósito si está pausado", async function () {
      await vault.connect(owner).pause();
      await expect(
        vault.connect(alice).deposit(ethers.parseUnits("10", 6), fakeView, fakeSpend)
      ).to.be.revertedWithCustomError(vault, "EnforcedPause");
    });
  });

  describe("Retiros V2 (EIP-712 Primario & Fallback Dual)", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    beforeEach(async function () {
      // Fondear la bóveda con 5,000 USDT de Alice
      await vault.connect(alice).deposit(ethers.parseUnits("5000", 6), fakeView, fakeSpend);
    });

    async function getEIP712Signature(orderId, recipient, amount, fee, signer, chainIdOverride, vaultOverride) {
      const network = await ethers.provider.getNetwork();
      const domain = {
        name: "CryptoPegVault",
        version: "2",
        chainId: chainIdOverride !== undefined ? chainIdOverride : network.chainId,
        verifyingContract: vaultOverride !== undefined ? vaultOverride : await vault.getAddress(),
      };
      const types = {
        Withdrawal: [
          { name: "orderId", type: "bytes32" },
          { name: "recipient", type: "address" },
          { name: "amount", type: "uint256" },
          { name: "fee", type: "uint256" },
        ],
      };
      const value = {
        orderId,
        recipient,
        amount,
        fee,
      };
      return await signer.signTypedData(domain, types, value);
    }

    async function getLegacySignature(orderId, recipient, amount, signer, chainIdOverride, vaultOverride) {
      const network = await ethers.provider.getNetwork();
      const cId = chainIdOverride !== undefined ? chainIdOverride : network.chainId;
      const vAddr = vaultOverride !== undefined ? vaultOverride : await vault.getAddress();
      const hash = ethers.solidityPackedKeccak256(
        ["bytes32", "address", "uint256", "uint256", "address"],
        [orderId, recipient, amount, cId, vAddr]
      );
      return await signer.signMessage(ethers.getBytes(hash));
    }

    it("debe ejecutar retiro con firma tipada EIP-712 válida y acumular comisiones", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("300", 6);
      const fee = ethers.parseUnits("1.5", 6);
      const sig = await getEIP712Signature(orderId, bob.address, amount, fee, validator);

      const feesBefore = await vault.accumulatedFees();
      const bobBefore = await usdt.balanceOf(bob.address);

      await expect(vault.connect(bob).withdraw(orderId, bob.address, amount, fee, sig))
        .to.emit(vault, "WithdrawalExecuted")
        .withArgs(orderId, bob.address, amount, fee, (v) => true);

      expect(await usdt.balanceOf(bob.address) - bobBefore).to.equal(amount);
      expect(await vault.accumulatedFees() - feesBefore).to.equal(fee);
      expect(await vault.executedWithdrawals(orderId)).to.be.true;
    });

    it("debe ejecutar retiro mediante fallback dual legado (eth_sign) si fee == 0", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("150", 6);
      const legacySig = await getLegacySignature(orderId, bob.address, amount, validator);

      const bobBefore = await usdt.balanceOf(bob.address);
      await expect(vault.connect(bob).withdraw(orderId, bob.address, amount, 0, legacySig))
        .to.emit(vault, "WithdrawalExecuted")
        .withArgs(orderId, bob.address, amount, 0, (v) => true);

      expect(await usdt.balanceOf(bob.address) - bobBefore).to.equal(amount);
    });

    it("debe rechazar fallback legado si fee > 0", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("150", 6);
      const legacySig = await getLegacySignature(orderId, bob.address, amount, validator);

      // 0.5 USDT respeta el límite de comisión (maxFee ~0.75 USDT) pero debe ser rechazado por ser firma legada
      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, ethers.parseUnits("0.5", 6), legacySig)
      ).to.be.revertedWith("Legacy signature does not authorize non-zero fee");
    });

    it("debe prevenir doble retiro (Replay Attack) con mismo orderId", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await getEIP712Signature(orderId, bob.address, amount, 0, validator);

      await vault.connect(bob).withdraw(orderId, bob.address, amount, 0, sig);
      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, 0, sig)
      ).to.be.revertedWith("Withdrawal order already executed");
    });

    it("debe rechazar retiro si recipient es dirección cero", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await getEIP712Signature(orderId, ethers.ZeroAddress, amount, 0, validator);

      await expect(
        vault.connect(bob).withdraw(orderId, ethers.ZeroAddress, amount, 0, sig)
      ).to.be.revertedWith("Invalid recipient address");
    });

    it("debe rechazar retiro con monto cero", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const sig = await getEIP712Signature(orderId, bob.address, 0, 0, validator);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, 0, 0, sig)
      ).to.be.revertedWith("Withdraw amount must be > 0");
    });

    it("debe rechazar retiro si la comisión supera la tasa máxima permitida por withdrawFeeBps (CP-HIGH-01)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      // WITHDRAW_FEE_BPS es 50 (0.50%). Para 100 USDT, maxFee es aprox 0.5025 USDT.
      // Intentamos cobrar una comisión inflada de 10 USDT
      const excessiveFee = ethers.parseUnits("10", 6);
      const sig = await getEIP712Signature(orderId, bob.address, amount, excessiveFee, validator);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, excessiveFee, sig)
      ).to.be.revertedWith("Withdrawal fee exceeds allowed rate");
    });

    it("debe rechazar firma de un atacante no autorizado", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const evilSig = await getEIP712Signature(orderId, bob.address, amount, 0, attacker);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, 0, evilSig)
      ).to.be.revertedWith("Invalid signature: EIP-712 and legacy verification failed");
    });

    it("debe rechazar firma EIP-712 para otra chainId (Cross-Chain Replay)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const crossChainSig = await getEIP712Signature(orderId, bob.address, amount, 0, validator, 999999);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, 0, crossChainSig)
      ).to.be.revertedWith("Invalid signature: EIP-712 and legacy verification failed");
    });

    it("debe rechazar firma EIP-712 para otro contrato (Cross-Contract Replay)", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const crossVaultSig = await getEIP712Signature(orderId, bob.address, amount, 0, validator, undefined, attacker.address);

      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, 0, crossVaultSig)
      ).to.be.revertedWith("Invalid signature: EIP-712 and legacy verification failed");
    });

    it("debe revertir retiro si el contrato está pausado", async function () {
      const orderId = ethers.hexlify(ethers.randomBytes(32));
      const amount = ethers.parseUnits("100", 6);
      const sig = await getEIP712Signature(orderId, bob.address, amount, 0, validator);

      await vault.connect(owner).pause();
      await expect(
        vault.connect(bob).withdraw(orderId, bob.address, amount, 0, sig)
      ).to.be.revertedWithCustomError(vault, "EnforcedPause");
    });
  });

  describe("Administración V2 & Tesorería", function () {
    const fakeView = ethers.hexlify(ethers.randomBytes(32));
    const fakeSpend = ethers.hexlify(ethers.randomBytes(32));

    beforeEach(async function () {
      await vault.connect(alice).deposit(ethers.parseUnits("10000", 6), fakeView, fakeSpend);
    });

    it("debe permitir al Owner reclamar comisiones de tesorería", async function () {
      const accFees = await vault.accumulatedFees();
      const claimAmount = ethers.parseUnits("25", 6);

      await expect(vault.connect(owner).claimTreasuryFees(treasury.address, claimAmount))
        .to.emit(vault, "TreasuryFeesClaimed")
        .withArgs(treasury.address, claimAmount, accFees - claimAmount, (v) => true);

      expect(await usdt.balanceOf(treasury.address)).to.equal(claimAmount);
      expect(await vault.accumulatedFees()).to.equal(accFees - claimAmount);
    });

    it("debe rechazar reclamo por no-propietario", async function () {
      await expect(
        vault.connect(attacker).claimTreasuryFees(attacker.address, ethers.parseUnits("10", 6))
      ).to.be.revertedWithCustomError(vault, "OwnableUnauthorizedAccount");
    });

    it("debe rechazar reclamo con destino cero o monto cero", async function () {
      await expect(
        vault.connect(owner).claimTreasuryFees(ethers.ZeroAddress, ethers.parseUnits("10", 6))
      ).to.be.revertedWith("Invalid treasury destination");
      await expect(
        vault.connect(owner).claimTreasuryFees(treasury.address, 0)
      ).to.be.revertedWith("Claim amount must be > 0");
    });

    it("debe rechazar reclamo superior a comisiones acumuladas", async function () {
      const accFees = await vault.accumulatedFees();
      await expect(
        vault.connect(owner).claimTreasuryFees(treasury.address, accFees + 1n)
      ).to.be.revertedWith("Requested amount exceeds accumulated fees");
    });

    it("debe permitir al Owner actualizar comisiones vía setFeeRates", async function () {
      await expect(vault.connect(owner).setFeeRates(75, 75))
        .to.emit(vault, "FeeRatesUpdated")
        .withArgs(75, 75);

      expect(await vault.depositFeeBps()).to.equal(75);
      expect(await vault.withdrawFeeBps()).to.equal(75);
    });

    it("debe rechazar setFeeRates con comisiones > 1000 bps", async function () {
      await expect(vault.connect(owner).setFeeRates(1001, 50)).to.be.revertedWith("Fee cannot exceed 10%");
      await expect(vault.connect(owner).setFeeRates(50, 1001)).to.be.revertedWith("Fee cannot exceed 10%");
    });

    it("debe permitir al Owner actualizar validatorSigner", async function () {
      const newValidator = bob.address;
      await expect(vault.connect(owner).setValidatorSigner(newValidator))
        .to.emit(vault, "ValidatorSignerUpdated")
        .withArgs(validator.address, newValidator);

      expect(await vault.validatorSigner()).to.equal(newValidator);
    });

    it("debe rechazar setValidatorSigner con dirección cero", async function () {
      await expect(
        vault.connect(owner).setValidatorSigner(ethers.ZeroAddress)
      ).to.be.revertedWith("New signer cannot be 0");
    });

    it("debe pausar y despausar solo por el Owner", async function () {
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
      const VaultFactoryV2 = await ethers.getContractFactory("CryptoPegVaultV2");
      const emptyVault = await VaultFactoryV2.deploy(await usdt.getAddress(), validator.address, 50, 50, owner.address);
      expect(await emptyVault.getCirculatingBacking()).to.equal(0);
    });
  });
});
