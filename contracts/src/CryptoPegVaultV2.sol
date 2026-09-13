// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@openzeppelin/contracts/utils/ReentrancyGuard.sol";
import "@openzeppelin/contracts/utils/Pausable.sol";
import "@openzeppelin/contracts/utils/cryptography/ECDSA.sol";
import "@openzeppelin/contracts/utils/cryptography/EIP712.sol";
import "@openzeppelin/contracts/utils/cryptography/MessageHashUtils.sol";

/**
 * @title CryptoPegVaultV2
 * @notice Contrato inteligente de custodia colateral 1:1 de USDT para el protocolo CryptoPeg (Versión 2).
 *         Incorpora firmas tipadas estructuradas bajo el estándar EIP-712 con fallback de compatibilidad dual,
 *         así como acumulación estricta de comisiones de retiro hacia la tesorería.
 */
contract CryptoPegVaultV2 is Ownable, ReentrancyGuard, Pausable, EIP712 {
    using SafeERC20 for IERC20;

    IERC20 public immutable usdtToken;
    address public validatorSigner;

    uint256 public depositFeeBps;   // Ejemplo: 50 = 0.50%
    uint256 public withdrawFeeBps;  // Ejemplo: 50 = 0.50%
    uint256 public accumulatedFees; // Reserva de comisiones en micro-unidades (6 decimales)

    // Prevención estricta de repetición (Replay Attack) en órdenes de retiro
    mapping(bytes32 => bool) public executedWithdrawals;

    // TypeHash EIP-712 canónico para órdenes de retiro estructuradas
    bytes32 public constant WITHDRAWAL_TYPEHASH = keccak256(
        "Withdrawal(bytes32 orderId,address recipient,uint256 amount,uint256 fee)"
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

    event WithdrawalExecuted(
        bytes32 indexed orderId,
        address indexed recipient,
        uint256 amount,
        uint256 fee,
        uint256 timestamp
    );

    event TreasuryFeesClaimed(
        address indexed treasuryDestination,
        uint256 amountClaimed,
        uint256 remainingFees,
        uint256 timestamp
    );

    event FeeRatesUpdated(uint256 newDepositFeeBps, uint256 newWithdrawFeeBps);
    event ValidatorSignerUpdated(address indexed oldSigner, address indexed newSigner);

    constructor(
        address _usdtToken,
        address _validatorSigner,
        uint256 _depositFeeBps,
        uint256 _withdrawFeeBps,
        address _initialOwner
    ) Ownable(_initialOwner) EIP712("CryptoPegVault", "2") {
        require(_usdtToken != address(0), "USDT address cannot be 0");
        require(_validatorSigner != address(0), "Validator signer cannot be 0");
        require(_depositFeeBps <= 1000 && _withdrawFeeBps <= 1000, "Fee cannot exceed 10%");

        usdtToken = IERC20(_usdtToken);
        validatorSigner = _validatorSigner;
        depositFeeBps = _depositFeeBps;
        withdrawFeeBps = _withdrawFeeBps;
    }

    /**
     * @notice Depositar USDT para acuñar tokens privados en la red confidencial CryptoPeg.
     * @param grossAmount Monto bruto de USDT a transferir (6 decimales).
     * @param stealthPubView Clave pública de vista (View Key) del destinatario furtivo.
     * @param stealthPubSpend Clave pública de gasto (Spend Key) del destinatario furtivo.
     */
    function deposit(
        uint256 grossAmount,
        bytes32 stealthPubView,
        bytes32 stealthPubSpend
    ) external nonReentrant whenNotPaused {
        require(grossAmount > 0, "Deposit amount must be > 0");
        require(stealthPubView != bytes32(0), "Invalid stealth view key");
        require(stealthPubSpend != bytes32(0), "Invalid stealth spend key");

        // Cálculo de comisión en punto fijo: (grossAmount * bps) / 10000
        uint256 fee = (grossAmount * depositFeeBps) / 10000;
        uint256 netMinted = grossAmount - fee;

        accumulatedFees += fee;

        // Transferencia segura de USDT desde la cuenta del usuario a la bóveda
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
     * @notice Ejecutar retiro hacia una dirección pública con firma tipada EIP-712 y fallback dual legado.
     * @param orderId Identificador único de la orden de retiro generado tras la quema confidencial.
     * @param recipient Dirección pública destino en la red L2 (Arbitrum).
     * @param amount Monto neto a dispersar al destinatario (en micro-USDT con 6 decimales).
     * @param fee Comisión de retiro retenida para la tesorería.
     * @param signature Firma ECDSA del validador autorizando el retiro.
     */
    function withdraw(
        bytes32 orderId,
        address recipient,
        uint256 amount,
        uint256 fee,
        bytes calldata signature
    ) external nonReentrant whenNotPaused {
        require(!executedWithdrawals[orderId], "Withdrawal order already executed");
        require(recipient != address(0), "Invalid recipient address");
        require(amount > 0, "Withdraw amount must be > 0");

        // 1. Verificación primaria: Firma tipada estructurada EIP-712
        bytes32 structHash = keccak256(
            abi.encode(WITHDRAWAL_TYPEHASH, orderId, recipient, amount, fee)
        );
        bytes32 digest = _hashTypedDataV4(structHash);
        address recovered = ECDSA.recover(digest, signature);

        // 2. Fallback de compatibilidad dual: Si la firma no coincide con EIP-712, verificar eth_sign legado
        if (recovered != validatorSigner) {
            bytes32 legacyHash = keccak256(
                abi.encodePacked(orderId, recipient, amount, block.chainid, address(this))
            );
            bytes32 ethSignedHash = MessageHashUtils.toEthSignedMessageHash(legacyHash);
            recovered = ECDSA.recover(ethSignedHash, signature);
            require(recovered == validatorSigner, "Invalid signature: EIP-712 and legacy verification failed");
        }

        executedWithdrawals[orderId] = true;

        // Acumular la comisión de retiro en la tesorería (AUD-HIGH-01)
        if (fee > 0) {
            accumulatedFees += fee;
        }

        // Transferir los USDT netos al beneficiario final
        usdtToken.safeTransfer(recipient, amount);

        emit WithdrawalExecuted(orderId, recipient, amount, fee, block.timestamp);
    }

    /**
     * @notice Extraer comisiones acumuladas por la tesorería sin comprometer el colateral 1:1.
     * @param treasuryDestination Billetera destinataria de los fondos de comisión.
     * @param amount Monto de comisiones a retirar.
     */
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

    function setValidatorSigner(address newSigner) external onlyOwner {
        require(newSigner != address(0), "New signer cannot be 0");
        address oldSigner = validatorSigner;
        validatorSigner = newSigner;
        emit ValidatorSignerUpdated(oldSigner, newSigner);
    }

    function pause() external onlyOwner {
        _pause();
    }

    function unpause() external onlyOwner {
        _unpause();
    }
}
