const { expect } = require("chai");
const { ethers } = require("hardhat");

describe("CryptoPegVaultV3 - quorum finality security", function () {
  let usdt, vault;
  let owner, validatorA, validatorB, validatorC, alice, bob, attacker, treasury;

  const DEPOSIT_FEE_BPS = 50;
  const WITHDRAW_FEE_BPS = 50;

  beforeEach(async function () {
    [owner, validatorA, validatorB, validatorC, alice, bob, attacker, treasury] = await ethers.getSigners();

    const MockUSDT = await ethers.getContractFactory("MockUSDT");
    usdt = await MockUSDT.deploy();
    await usdt.waitForDeployment();

    const Vault = await ethers.getContractFactory("CryptoPegVaultV3");
    vault = await Vault.deploy(
      await usdt.getAddress(),
      [validatorA.address, validatorB.address, validatorC.address],
      2,
      DEPOSIT_FEE_BPS,
      WITHDRAW_FEE_BPS,
      owner.address
    );
    await vault.waitForDeployment();

    await usdt.mint(alice.address, ethers.parseUnits("10000", 6));
    await usdt.connect(alice).approve(await vault.getAddress(), ethers.MaxUint256);

    const view = ethers.hexlify(ethers.randomBytes(32));
    const spend = ethers.hexlify(ethers.randomBytes(32));
    await vault.connect(alice).deposit(ethers.parseUnits("5000", 6), view, spend);
  });

  function checkpointTypes() {
    return {
      Checkpoint: [
        { name: "height", type: "uint64" },
        { name: "blockHash", type: "bytes32" },
        { name: "parentCheckpointHash", type: "bytes32" },
      ],
    };
  }

  function withdrawalTypes() {
    return {
      Withdrawal: [
        { name: "orderId", type: "bytes32" },
        { name: "recipient", type: "address" },
        { name: "amount", type: "uint256" },
        { name: "fee", type: "uint256" },
        { name: "withdrawalBlockHeight", type: "uint64" },
        { name: "withdrawalBlockHash", type: "bytes32" },
        { name: "checkpointHeight", type: "uint64" },
        { name: "checkpointHash", type: "bytes32" },
      ],
    };
  }

  async function domain(chainIdOverride, contractOverride) {
    const network = await ethers.provider.getNetwork();
    return {
      name: "CryptoPegVault",
      version: "3",
      chainId: chainIdOverride !== undefined ? chainIdOverride : network.chainId,
      verifyingContract: contractOverride !== undefined ? contractOverride : await vault.getAddress(),
    };
  }

  async function signCheckpoint(signer, height, blockHash, parentHash, domainOverride) {
    return signer.signTypedData(
      domainOverride || await domain(),
      checkpointTypes(),
      { height, blockHash, parentCheckpointHash: parentHash }
    );
  }

  async function signWithdrawal(signer, value, domainOverride) {
    return signer.signTypedData(
      domainOverride || await domain(),
      withdrawalTypes(),
      value
    );
  }

  async function finalize(height = 100n, blockHash = ethers.hexlify(ethers.randomBytes(32))) {
    const parent = await vault.latestFinalizedBlockHash();
    const sigA = await signCheckpoint(validatorA, height, blockHash, parent);
    const sigB = await signCheckpoint(validatorB, height, blockHash, parent);
    await vault.finalizeCheckpoint(height, blockHash, parent, [sigA, sigB]);
    return { height, blockHash };
  }

  describe("validator set", function () {
    it("requires at least 3 validators and a strict-majority threshold", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVaultV3");
      await expect(
        Vault.deploy(await usdt.getAddress(), [validatorA.address, validatorB.address], 2, 50, 50, owner.address)
      ).to.be.revertedWith("At least 3 validators required");

      await expect(
        Vault.deploy(
          await usdt.getAddress(),
          [validatorA.address, validatorB.address, validatorC.address],
          1,
          50,
          50,
          owner.address
        )
      ).to.be.revertedWith("Invalid validator threshold");
    });

    it("rejects duplicate validators", async function () {
      const Vault = await ethers.getContractFactory("CryptoPegVaultV3");
      await expect(
        Vault.deploy(
          await usdt.getAddress(),
          [validatorA.address, validatorA.address, validatorC.address],
          2,
          50,
          50,
          owner.address
        )
      ).to.be.revertedWith("Duplicate validator");
    });

    it("owner cannot replace the validator set", async function () {
      expect(await vault.isValidator(validatorA.address)).to.equal(true);
      expect(await vault.isValidator(attacker.address)).to.equal(false);
      expect(await vault.validatorThreshold()).to.equal(2n);
      expect(vault.interface.getFunction("setValidatorSigner")).to.equal(null);
    });
  });

  describe("checkpoint finality", function () {
    it("rejects a checkpoint signed by only one validator", async function () {
      const height = 100n;
      const hash = ethers.hexlify(ethers.randomBytes(32));
      const parent = ethers.ZeroHash;
      const sig = await signCheckpoint(validatorA, height, hash, parent);
      await expect(vault.finalizeCheckpoint(height, hash, parent, [sig]))
        .to.be.revertedWith("Insufficient validator quorum");
    });

    it("finalizes with 2-of-3 independent signatures", async function () {
      const height = 100n;
      const hash = ethers.hexlify(ethers.randomBytes(32));
      const parent = ethers.ZeroHash;
      const sigA = await signCheckpoint(validatorA, height, hash, parent);
      const sigB = await signCheckpoint(validatorB, height, hash, parent);

      await expect(vault.finalizeCheckpoint(height, hash, parent, [sigA, sigB]))
        .to.emit(vault, "CheckpointFinalized")
        .withArgs(height, hash, parent, 2n);

      expect(await vault.latestFinalizedHeight()).to.equal(height);
      expect(await vault.latestFinalizedBlockHash()).to.equal(hash);
    });

    it("rejects duplicate signatures masquerading as quorum", async function () {
      const height = 100n;
      const hash = ethers.hexlify(ethers.randomBytes(32));
      const sigA = await signCheckpoint(validatorA, height, hash, ethers.ZeroHash);
      await expect(vault.finalizeCheckpoint(height, hash, ethers.ZeroHash, [sigA, sigA]))
        .to.be.revertedWith("Duplicate validator signature");
    });

    it("cannot replace a finalized checkpoint at the same height", async function () {
      await finalize(100n);
      const competing = ethers.hexlify(ethers.randomBytes(32));
      const current = await vault.latestFinalizedBlockHash();
      const sigA = await signCheckpoint(validatorA, 100n, competing, current);
      const sigB = await signCheckpoint(validatorB, 100n, competing, current);
      await expect(vault.finalizeCheckpoint(100n, competing, current, [sigA, sigB]))
        .to.be.revertedWith("Checkpoint height must advance");
    });

    it("new checkpoints must link to the previous finalized checkpoint", async function () {
      await finalize(100n);
      const nextHash = ethers.hexlify(ethers.randomBytes(32));
      const wrongParent = ethers.hexlify(ethers.randomBytes(32));
      const sigA = await signCheckpoint(validatorA, 110n, nextHash, wrongParent);
      const sigB = await signCheckpoint(validatorB, 110n, nextHash, wrongParent);
      await expect(vault.finalizeCheckpoint(110n, nextHash, wrongParent, [sigA, sigB]))
        .to.be.revertedWith("Checkpoint parent mismatch");
    });
  });

  describe("withdrawals bound to finalized checkpoints", function () {
    it("rejects withdrawals before any checkpoint is finalized", async function () {
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 10n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: 0n,
        checkpointHash: ethers.ZeroHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      const sigB = await signWithdrawal(validatorB, value);
      await expect(
        vault.withdraw(
          value.orderId,
          value.recipient,
          value.amount,
          value.fee,
          value.withdrawalBlockHeight,
          value.withdrawalBlockHash,
          value.checkpointHeight,
          value.checkpointHash,
          [sigA, sigB]
        )
      ).to.be.revertedWith("No finalized checkpoint");
    });

    it("one compromised validator cannot release USDT", async function () {
      const cp = await finalize(100n);
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 95n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp.height,
        checkpointHash: cp.blockHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      await expect(
        vault.withdraw(
          value.orderId,
          value.recipient,
          value.amount,
          value.fee,
          value.withdrawalBlockHeight,
          value.withdrawalBlockHash,
          value.checkpointHeight,
          value.checkpointHash,
          [sigA]
        )
      ).to.be.revertedWith("Insufficient validator quorum");
    });

    it("releases USDT only with quorum signatures bound to the current checkpoint", async function () {
      const cp = await finalize(100n);
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("300", 6),
        fee: ethers.parseUnits("1", 6),
        withdrawalBlockHeight: 95n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp.height,
        checkpointHash: cp.blockHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      const sigC = await signWithdrawal(validatorC, value);
      const before = await usdt.balanceOf(bob.address);

      await vault.withdraw(
        value.orderId,
        value.recipient,
        value.amount,
        value.fee,
        value.withdrawalBlockHeight,
        value.withdrawalBlockHash,
        value.checkpointHeight,
        value.checkpointHash,
        [sigA, sigC]
      );

      expect((await usdt.balanceOf(bob.address)) - before).to.equal(value.amount);
      expect(await vault.executedWithdrawals(value.orderId)).to.equal(true);
    });

    it("rejects a withdrawal bound to a stale or competing checkpoint", async function () {
      const cp1 = await finalize(100n);
      const cp2Hash = ethers.hexlify(ethers.randomBytes(32));
      const sigCA = await signCheckpoint(validatorA, 110n, cp2Hash, cp1.blockHash);
      const sigCB = await signCheckpoint(validatorB, 110n, cp2Hash, cp1.blockHash);
      await vault.finalizeCheckpoint(110n, cp2Hash, cp1.blockHash, [sigCA, sigCB]);

      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 95n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp1.height,
        checkpointHash: cp1.blockHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      const sigB = await signWithdrawal(validatorB, value);
      await expect(
        vault.withdraw(
          value.orderId,
          value.recipient,
          value.amount,
          value.fee,
          value.withdrawalBlockHeight,
          value.withdrawalBlockHash,
          value.checkpointHeight,
          value.checkpointHash,
          [sigA, sigB]
        )
      ).to.be.revertedWith("Checkpoint height is not current");
    });

    it("prevents replay of an already executed withdrawal", async function () {
      const cp = await finalize(100n);
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 90n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp.height,
        checkpointHash: cp.blockHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      const sigB = await signWithdrawal(validatorB, value);
      const args = [
        value.orderId,
        value.recipient,
        value.amount,
        value.fee,
        value.withdrawalBlockHeight,
        value.withdrawalBlockHash,
        value.checkpointHeight,
        value.checkpointHash,
        [sigA, sigB],
      ];
      await vault.withdraw(...args);
      await expect(vault.withdraw(...args)).to.be.revertedWith("Withdrawal order already executed");
    });

    it("rejects an unauthorized signer even when paired with one valid validator", async function () {
      const cp = await finalize(100n);
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 90n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp.height,
        checkpointHash: cp.blockHash,
      };
      const sigA = await signWithdrawal(validatorA, value);
      const evilSig = await signWithdrawal(attacker, value);
      await expect(
        vault.withdraw(
          value.orderId,
          value.recipient,
          value.amount,
          value.fee,
          value.withdrawalBlockHeight,
          value.withdrawalBlockHash,
          value.checkpointHeight,
          value.checkpointHash,
          [sigA, evilSig]
        )
      ).to.be.revertedWith("Unauthorized validator signature");
    });

    it("EIP-712 domain prevents cross-contract authorization replay", async function () {
      const cp = await finalize(100n);
      const value = {
        orderId: ethers.hexlify(ethers.randomBytes(32)),
        recipient: bob.address,
        amount: ethers.parseUnits("100", 6),
        fee: 0n,
        withdrawalBlockHeight: 90n,
        withdrawalBlockHash: ethers.hexlify(ethers.randomBytes(32)),
        checkpointHeight: cp.height,
        checkpointHash: cp.blockHash,
      };
      const wrongDomain = await domain(undefined, attacker.address);
      const sigA = await signWithdrawal(validatorA, value, wrongDomain);
      const sigB = await signWithdrawal(validatorB, value, wrongDomain);
      await expect(
        vault.withdraw(
          value.orderId,
          value.recipient,
          value.amount,
          value.fee,
          value.withdrawalBlockHeight,
          value.withdrawalBlockHash,
          value.checkpointHeight,
          value.checkpointHash,
          [sigA, sigB]
        )
      ).to.be.revertedWith("Unauthorized validator signature");
    });
  });

  describe("treasury separation", function () {
    it("owner can claim only accumulated fees, not user backing", async function () {
      const fees = await vault.accumulatedFees();
      await expect(vault.connect(owner).claimTreasuryFees(treasury.address, fees + 1n))
        .to.be.revertedWith("Requested amount exceeds accumulated fees");
    });
  });
});
