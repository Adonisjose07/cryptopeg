// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@openzeppelin/contracts/utils/ReentrancyGuard.sol";
import "@openzeppelin/contracts/utils/Pausable.sol";
import "@openzeppelin/contracts/utils/cryptography/ECDSA.sol";
import "@openzeppelin/contracts/utils/cryptography/EIP712.sol";

/**
 * @title CryptoPegVaultV3
 * @notice Bridge vault hardened against single-signer compromise and private-chain fork releases.
 *
 * Security model:
 *  - Withdrawals require an M-of-N EIP-712 validator quorum.
 *  - M must be a strict majority, guaranteeing quorum intersection.
 *  - Validator membership is immutable for this deployment: the owner cannot replace the
 *    validator set and turn the bridge back into a single-key trust model.
 *  - A withdrawal is valid only against the latest on-chain finalized private-chain checkpoint.
 *  - Checkpoints advance monotonically and are themselves authorized by M-of-N signatures.
 *  - Validators/cosigners MUST independently verify that a proposed checkpoint extends the
 *    current finalized checkpoint before signing it.
 */
contract CryptoPegVaultV3 is Ownable, ReentrancyGuard, Pausable, EIP712 {
    using SafeERC20 for IERC20;

    IERC20 public immutable usdtToken;

    uint256 public depositFeeBps;
    uint256 public withdrawFeeBps;
    uint256 public accumulatedFees;

    mapping(address => bool) public isValidator;
    address[] private _validators;
    uint256 public immutable validatorThreshold;

    uint64 public latestFinalizedHeight;
    bytes32 public latestFinalizedBlockHash;

    mapping(bytes32 => bool) public executedWithdrawals;

    bytes32 public constant CHECKPOINT_TYPEHASH = keccak256(
        "Checkpoint(uint64 height,bytes32 blockHash,bytes32 parentCheckpointHash)"
    );

    bytes32 public constant WITHDRAWAL_TYPEHASH = keccak256(
        "Withdrawal(bytes32 orderId,address recipient,uint256 amount,uint256 fee,uint64 withdrawalBlockHeight,bytes32 withdrawalBlockHash,uint64 checkpointHeight,bytes32 checkpointHash)"
    );

    event DepositInitiated(
        address indexed depositor,
        uint256 grossAmount,
        uint256 netMinted,
        uint256 fee,
        bytes32 stealthPubView,
        bytes32 stealthPubSpend,
        uint256 timestamp
    );

    event CheckpointFinalized(
        uint64 indexed height,
        bytes32 indexed blockHash,
        bytes32 indexed parentCheckpointHash,
        uint256 quorumSize
    );

    event WithdrawalExecuted(
        bytes32 indexed orderId,
        address indexed recipient,
        uint256 amount,
        uint256 fee,
        uint64 withdrawalBlockHeight,
        bytes32 withdrawalBlockHash,
        uint64 checkpointHeight,
        bytes32 checkpointHash,
        uint256 timestamp
    );

    event TreasuryFeesClaimed(
        address indexed treasuryDestination,
        uint256 amountClaimed,
        uint256 remainingFees,
        uint256 timestamp
    );

    event FeeRatesUpdated(uint256 newDepositFeeBps, uint256 newWithdrawFeeBps);

    constructor(
        address _usdtToken,
        address[] memory validators_,
        uint256 threshold_,
        uint256 _depositFeeBps,
        uint256 _withdrawFeeBps,
        address _initialOwner
    ) Ownable(_initialOwner) EIP712("CryptoPegVault", "3") {
        require(_usdtToken != address(0), "USDT address cannot be 0");
        require(validators_.length >= 3, "At least 3 validators required");
        require(threshold_ >= 2 && threshold_ <= validators_.length, "Invalid validator threshold");
        require(threshold_ * 2 > validators_.length, "Threshold must be strict majority");
        require(_depositFeeBps <= 1000 && _withdrawFeeBps <= 1000, "Fee cannot exceed 10%");

        for (uint256 i = 0; i < validators_.length; ++i) {
            address validator = validators_[i];
            require(validator != address(0), "Validator cannot be 0");
            require(!isValidator[validator], "Duplicate validator");
            isValidator[validator] = true;
            _validators.push(validator);
        }

        usdtToken = IERC20(_usdtToken);
        validatorThreshold = threshold_;
        depositFeeBps = _depositFeeBps;
        withdrawFeeBps = _withdrawFeeBps;
    }

    function getValidators() external view returns (address[] memory) {
        return _validators;
    }

    function validatorCount() external view returns (uint256) {
        return _validators.length;
    }

    function deposit(
        uint256 grossAmount,
        bytes32 stealthPubView,
        bytes32 stealthPubSpend
    ) external nonReentrant whenNotPaused {
        require(grossAmount > 0, "Deposit amount must be > 0");
        require(stealthPubView != bytes32(0), "Invalid stealth view key");
        require(stealthPubSpend != bytes32(0), "Invalid stealth spend key");

        uint256 fee = (grossAmount * depositFeeBps) / 10000;
        uint256 netMinted = grossAmount - fee;
        accumulatedFees += fee;

        usdtToken.safeTransferFrom(msg.sender, address(this), grossAmount);

        emit DepositInitiated(
            msg.sender,
            grossAmount,
            netMinted,
            fee,
            stealthPubView,
            stealthPubSpend,
            block.timestamp
        );
    }

    /**
     * @notice Finalize one canonical private-chain checkpoint on Arbitrum.
     * @dev Validators must independently verify ancestry off-chain before signing. The contract
     * enforces monotonicity and a hash-chain of checkpoint certificates so a single signer or
     * relayer cannot choose an alternate fork.
     */
    function finalizeCheckpoint(
        uint64 height,
        bytes32 blockHash,
        bytes32 parentCheckpointHash,
        bytes[] calldata signatures
    ) external whenNotPaused {
        require(blockHash != bytes32(0), "Checkpoint block hash cannot be 0");
        require(height > latestFinalizedHeight, "Checkpoint height must advance");
        require(parentCheckpointHash == latestFinalizedBlockHash, "Checkpoint parent mismatch");

        bytes32 structHash = keccak256(
            abi.encode(CHECKPOINT_TYPEHASH, height, blockHash, parentCheckpointHash)
        );
        bytes32 digest = _hashTypedDataV4(structHash);
        uint256 quorum = _verifyQuorum(digest, signatures);

        latestFinalizedHeight = height;
        latestFinalizedBlockHash = blockHash;

        emit CheckpointFinalized(height, blockHash, parentCheckpointHash, quorum);
    }

    /**
     * @notice Release USDT only for a withdrawal included in the canonical finalized chain.
     * @dev The contract cannot inspect the private chain itself. The independent validator quorum
     * attests both the withdrawal block and the currently finalized checkpoint. A relayer alone
     * holds no authority to release funds.
     */
    function withdraw(
        bytes32 orderId,
        address recipient,
        uint256 amount,
        uint256 fee,
        uint64 withdrawalBlockHeight,
        bytes32 withdrawalBlockHash,
        uint64 checkpointHeight,
        bytes32 checkpointHash,
        bytes[] calldata signatures
    ) external nonReentrant whenNotPaused {
        require(!executedWithdrawals[orderId], "Withdrawal order already executed");
        require(orderId != bytes32(0), "Invalid orderId");
        require(recipient != address(0), "Invalid recipient address");
        require(amount > 0, "Withdraw amount must be > 0");
        require(withdrawalBlockHash != bytes32(0), "Withdrawal block hash cannot be 0");
        require(latestFinalizedHeight > 0, "No finalized checkpoint");
        require(checkpointHeight == latestFinalizedHeight, "Checkpoint height is not current");
        require(checkpointHash == latestFinalizedBlockHash, "Checkpoint hash is not current");
        require(withdrawalBlockHeight <= checkpointHeight, "Withdrawal is not finalized");

        if (withdrawFeeBps == 0) {
            require(fee == 0, "Withdrawal fee must be 0 when fee rate is 0");
        } else {
            uint256 maxFee = (amount * withdrawFeeBps) / (10000 - withdrawFeeBps) + 1;
            require(fee <= maxFee, "Withdrawal fee exceeds allowed rate");
        }

        bytes32 structHash = keccak256(
            abi.encode(
                WITHDRAWAL_TYPEHASH,
                orderId,
                recipient,
                amount,
                fee,
                withdrawalBlockHeight,
                withdrawalBlockHash,
                checkpointHeight,
                checkpointHash
            )
        );
        bytes32 digest = _hashTypedDataV4(structHash);
        _verifyQuorum(digest, signatures);

        executedWithdrawals[orderId] = true;
        if (fee > 0) accumulatedFees += fee;

        usdtToken.safeTransfer(recipient, amount);

        emit WithdrawalExecuted(
            orderId,
            recipient,
            amount,
            fee,
            withdrawalBlockHeight,
            withdrawalBlockHash,
            checkpointHeight,
            checkpointHash,
            block.timestamp
        );
    }

    function _verifyQuorum(
        bytes32 digest,
        bytes[] calldata signatures
    ) internal view returns (uint256 uniqueCount) {
        require(signatures.length >= validatorThreshold, "Insufficient validator quorum");
        require(signatures.length <= _validators.length, "Too many validator signatures");

        address[] memory seen = new address[](signatures.length);

        for (uint256 i = 0; i < signatures.length; ++i) {
            address signer = ECDSA.recover(digest, signatures[i]);
            require(isValidator[signer], "Unauthorized validator signature");

            for (uint256 j = 0; j < uniqueCount; ++j) {
                require(seen[j] != signer, "Duplicate validator signature");
            }

            seen[uniqueCount] = signer;
            ++uniqueCount;
        }

        require(uniqueCount >= validatorThreshold, "Insufficient unique validator quorum");
    }

    function claimTreasuryFees(
        address treasuryDestination,
        uint256 amount
    ) external onlyOwner nonReentrant {
        require(treasuryDestination != address(0), "Invalid treasury destination");
        require(amount > 0, "Claim amount must be > 0");
        require(amount <= accumulatedFees, "Requested amount exceeds accumulated fees");

        accumulatedFees -= amount;
        usdtToken.safeTransfer(treasuryDestination, amount);

        emit TreasuryFeesClaimed(treasuryDestination, amount, accumulatedFees, block.timestamp);
    }

    function setFeeRates(uint256 newDepositFeeBps, uint256 newWithdrawFeeBps) external onlyOwner {
        require(newDepositFeeBps <= 1000 && newWithdrawFeeBps <= 1000, "Fee cannot exceed 10%");
        depositFeeBps = newDepositFeeBps;
        withdrawFeeBps = newWithdrawFeeBps;
        emit FeeRatesUpdated(newDepositFeeBps, newWithdrawFeeBps);
    }

    function pause() external onlyOwner {
        _pause();
    }

    function unpause() external onlyOwner {
        _unpause();
    }

    function getCollateralBalance() external view returns (uint256) {
        return usdtToken.balanceOf(address(this));
    }

    function getCirculatingBacking() external view returns (uint256) {
        uint256 total = usdtToken.balanceOf(address(this));
        if (total <= accumulatedFees) return 0;
        return total - accumulatedFees;
    }
}
