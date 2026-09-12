// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/ERC20.sol";

/**
 * @title MockUSDT
 * @notice Token de prueba ERC-20 simulando Tether USD (USDT) con 6 decimales para Arbitrum Sepolia.
 */
contract MockUSDT is ERC20 {
    uint8 private constant _DECIMALS = 6;

    constructor() ERC20("Tether USD", "USDT") {
        // Acuñar 10,000,000 USDT de prueba iniciales para el creador
        _mint(msg.sender, 10_000_000 * 10**_DECIMALS);
    }

    function decimals() public pure override returns (uint8) {
        return _DECIMALS;
    }

    /**
     * @notice Permite a cualquier tester o script acuñar tokens para pruebas en testnet
     */
    function mint(address to, uint256 amount) external {
        _mint(to, amount);
    }
}
