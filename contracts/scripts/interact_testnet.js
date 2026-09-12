const hre = require("hardhat");
const deployments = require("../deployments/arbitrumSepolia.json");

async function main() {
  console.log("===============================================================");
  console.log(" INTERACCIÓN EN VIVO CON SMART CONTRACT EN ARBITRUM SEPOLIA");
  console.log("===============================================================\n");

  const [deployer] = await hre.ethers.getSigners();
  console.log("Cuenta:", deployer.address);
  console.log("Gas ETH:", hre.ethers.formatEther(await hre.ethers.provider.getBalance(deployer.address)), "ETH");

  const usdtAddress = deployments.contracts.MockUSDT.address;
  const vaultAddress = deployments.contracts.CryptoPegVault.address;

  console.log("MockUSDT:       ", usdtAddress);
  console.log("CryptoPegVault: ", vaultAddress);

  const mockUsdt = await hre.ethers.getContractAt("MockUSDT", usdtAddress);
  const vault = await hre.ethers.getContractAt("CryptoPegVault", vaultAddress);

  // 1. Consultar balance de MockUSDT del deployer
  const balance = await mockUsdt.balanceOf(deployer.address);
  console.log("\n1. Balance actual de MockUSDT:", hre.ethers.formatUnits(balance, 6), "USDT");

  // 2. Aprobar 100 USDT al Vault
  console.log("\n2. Aprobando 100 USDT para la Bóveda CryptoPegVault...");
  const depositAmount = hre.ethers.parseUnits("100", 6);
  const approveTx = await mockUsdt.approve(vaultAddress, depositAmount);
  await approveTx.wait();
  console.log("   [OK] Aprobación confirmada en Arbiscan (Tx:", approveTx.hash, ")");

  // 3. Depositar 100 USDT en CryptoPegVault con claves furtivas de prueba
  console.log("\n3. Ejecutando depósito de 100 USDT en CryptoPegVault...");
  const fakeStealthView = hre.ethers.hexlify(hre.ethers.randomBytes(32));
  const fakeStealthSpend = hre.ethers.hexlify(hre.ethers.randomBytes(32));

  const depTx = await vault.deposit(depositAmount, fakeStealthView, fakeStealthSpend);
  const depReceipt = await depTx.wait();
  console.log("   [OK] Depósito procesado en bloque:", depReceipt.blockNumber, "(Tx:", depTx.hash, ")");

  // 4. Consultar estado on-chain de la Bóveda
  const vaultCollateral = await vault.getCollateralBalance();
  const circulating = await vault.getCirculatingBacking();
  const fees = await vault.accumulatedFees();

  console.log("\n4. Estado On-Chain de la Bóveda en Arbitrum Sepolia:");
  console.log("   - Colateral Total USDT:  ", hre.ethers.formatUnits(vaultCollateral, 6));
  console.log("   - Respaldo Circulante:   ", hre.ethers.formatUnits(circulating, 6));
  console.log("   - Comisiones Acumuladas: ", hre.ethers.formatUnits(fees, 6));
  console.log("   - 100% Solvente:         ", (vaultCollateral.toString() === (circulating + fees).toString()));

  console.log("\n===============================================================");
  console.log(" [EXITO] Transacciones verificables en vivo en:");
  console.log(" https://sepolia.arbiscan.io/address/" + vaultAddress);
  console.log("===============================================================\n");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
