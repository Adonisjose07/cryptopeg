// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@openzeppelin/contracts/utils/ReentrancyGuard.sol";
import "@openzeppelin/contracts/utils/Pausable.sol";
import "@openzeppelin/contracts/utils/cryptography/ECDSA.sol";
import "@openzeppelin/contracts/utils/cryptography/MessageHashUtils.sol";

/**
 * @title CryptoPegVault
 * @notice Contrato inteligente de custodia colateral 1:1 de USDT para el protocolo CryptoPeg.
 *         Diseñado para operar en redes Layer 2 de bajo costo como Arbitrum One / Arbitrum Sepolia.
 * 
 * Invariante de Solvencia:
 *     Balance USDT en Bóveda == Respaldo Circulante + Reserva de Comisiones Acumuladas
 */
contract CryptoPegVault is Ownable, ReentrancyGuard, Pausable {
    using SafeERC20 for IERC20;

    IERC20 public immutable usdtToken;
    address public validatorSigner;

    uint256 public depositFeeBps;   // Ejemplo: 50 = 0.50%
    uint256 public withdrawFeeBps;  // Ejemplo: 50 = 0.50%
    uint256 public accumulatedFees; // Reserva de comisiones en micro-unidades (6 decimales)

    // Prevención estricta de repetición (Replay Attack) en órdenes de retiro
    mapping(bytes32 => bool) public executedWithdrawals;

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
    ) Ownable(_initialOwner) {
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
     * @notice Ejecutar retiro hacia una dirección pública autorizada mediante firma del nodo validador.
     * @param orderId Identificador único de la orden de retiro generado tras la quema confidencial.
     * @param recipient Dirección pública destino en la red L2 (Arbitrum).
     * @param amount Monto neto a dispersar (en micro-USDT con 6 decimales).
     * @param signature Firma ECDSA del validador autorizando la quema de los tokens privados.
     */
    function withdraw(
        bytes32 orderId,
        address recipient,
        uint256 amount,
        bytes calldata signature
    ) external nonReentrant whenNotPaused {
        require(!executedWithdrawals[orderId], "Withdrawal order already executed");
        require(recipient != address(0), "Invalid recipient address");
        require(amount > 0, "Withdrawal amount must be > 0");

        // Construir el hash firmado tipado (incluye chainId y address(this) para prevenir replay cross-chain)
        bytes32 messageHash = keccak256(
            abi.encodePacked(orderId, recipient, amount, block.chainid, address(this))
        );
        bytes32 ethSignedMessageHash = MessageHashUtils.toEthSignedMessageHash(messageHash);

        address recoveredSigner = ECDSA.recover(ethSignedMessageHash, signature);
        require(recoveredSigner == validatorSigner, "Invalid validator signature for withdrawal");

        // Marcar orden como ejecutada inmediatamente antes de la transferencia
        executedWithdrawals[orderId] = true;

        // Acumular la comisión de retiro retenida en la bóveda para la tesorería (AUD-HIGH-01)
        if (withdrawFeeBps > 0 && withdrawFeeBps < 10000) {
            uint256 fee = (amount * withdrawFeeBps) / (10000 - withdrawFeeBps);
            accumulatedFees += fee;
        }

        // Transferir USDT al destinatario
        usdtToken.safeTransfer(recipient, amount);

        emit WithdrawalExecuted(orderId, recipient, amount, block.timestamp);
    }

    /**
     * @notice Cobrar comisiones acumuladas del protocolo hacia la billetera de tesorería.
     *         Restringido exclusivamente al propietario (Owner / Multisig).
     * @param treasuryDestination Billetera de tesorería destinataria.
     * @param amount Monto de comisiones a retirar (debe ser <= accumulatedFees).
     */
    function claimTreasuryFees(
        address treasuryDestination,
        uint256 amount
    ) external onlyOwner nonReentrant {
        require(treasuryDestination != address(0), "Invalid treasury destination address");
        require(amount > 0, "Claim amount must be > 0");
        require(amount <= accumulatedFees, "Amount exceeds available fee pool reserves");

        // Deducir de las comisiones acumuladas (el colateral circulante no se altera)
        accumulatedFees -= amount;

        usdtToken.safeTransfer(treasuryDestination, amount);

        emit TreasuryFeesClaimed(treasuryDestination, amount, accumulatedFees, block.timestamp);
    }

    /**
     * @notice Actualizar la dirección validadora del nodo.
     */
    function setValidatorSigner(address _newSigner) external onlyOwner {
        require(_newSigner != address(0), "New signer cannot be address 0");
        address oldSigner = validatorSigner;
        validatorSigner = _newSigner;
        emit ValidatorSignerUpdated(oldSigner, _newSigner);
    }

    /**
     * @notice Ajustar comisiones de depósito y retiro (máximo 10% = 1000 bps).
     */
    function setFees(uint256 _depositFeeBps, uint256 _withdrawFeeBps) external onlyOwner {
        require(_depositFeeBps <= 1000 && _withdrawFeeBps <= 1000, "Fee cannot exceed 10%");
        depositFeeBps = _depositFeeBps;
        withdrawFeeBps = _withdrawFeeBps;
        emit FeeRatesUpdated(_depositFeeBps, _withdrawFeeBps);
    }

    function pause() external onlyOwner {
        _pause();
    }

    function unpause() external onlyOwner {
        _unpause();
    }

    /**
     * @notice Consultar colateral total físico en la bóveda.
     */
    function getCollateralBalance() external view returns (uint256) {
        return usdtToken.balanceOf(address(this));
    }

    /**
     * @notice Consultar colateral correspondiente a tokens privados de usuarios en circulación.
     */
    function getCirculatingBacking() external view returns (uint256) {
        uint256 total = usdtToken.balanceOf(address(this));
        if (total <= accumulatedFees) return 0;
        return total - accumulatedFees;
    }
}
