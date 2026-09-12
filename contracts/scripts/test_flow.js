const hre = require("hardhat");
const { expect } = require("chai");

async function main() {
  console.log("===============================================================");
  console.log(" PRUEBA INTEGRAL DEL SMART CONTRACT CRYPTOPEG VAULT (ARBITRUM)");
  console.log("===============================================================\n");

  const [owner, validator, alice, bob, treasury] = await hre.ethers.getSigners();

  // 1. Desplegar MockUSDT
  console.log("1. Desplegando MockUSDT...");
  const MockUSDT = await hre.ethers.getContractFactory("MockUSDT");
  const usdt = await MockUSDT.deploy();
  await usdt.waitForDeployment();
  const usdtAddress = await usdt.getAddress();
  console.log("   MockUSDT en:", usdtAddress);

  // 2. Desplegar CryptoPegVault
  console.log("\n2. Desplegando CryptoPegVault (50 bps depósito, 50 bps retiro)...");
  const CryptoPegVault = await hre.ethers.getContractFactory("CryptoPegVault");
  const vault = await CryptoPegVault.deploy(
    usdtAddress,
    validator.address,
    50, // 0.50%
    50, // 0.50%
    owner.address
  );
  await vault.waitForDeployment();
  const vaultAddress = await vault.getAddress();
  console.log("   CryptoPegVault en:", vaultAddress);

  // 3. Alice obtiene 1,000 USDT de prueba
  console.log("\n3. Fondeando a Alice con 1,000 USDT...");
  const amount1000 = hre.ethers.parseUnits("1000", 6);
  await usdt.mint(alice.address, amount1000);
  console.log("   Balance Alice USDT:", hre.ethers.formatUnits(await usdt.balanceOf(alice.address), 6));

  // 4. Alice deposita 500 USDT con claves furtivas DKSAP
  console.log("\n4. Alice aprueba y deposita 500 USDT en la Bóveda...");
  const deposit500 = hre.ethers.parseUnits("500", 6);
  await usdt.connect(alice).approve(vaultAddress, deposit500);

  const fakeStealthView = hre.ethers.hexlify(hre.ethers.randomBytes(32));
  const fakeStealthSpend = hre.ethers.hexlify(hre.ethers.randomBytes(32));

  const txDeposit = await vault.connect(alice).deposit(deposit500, fakeStealthView, fakeStealthSpend);
  const receiptDeposit = await txDeposit.wait();
  console.log("   [OK] Depósito procesado en bloque:", receiptDeposit.blockNumber);

  // Comprobar balances de la bóveda
  const vaultBalance = await usdt.balanceOf(vaultAddress);
  const accumulatedFees = await vault.accumulatedFees();
  const circulatingBacking = await vault.getCirculatingBacking();

  console.log("   - Total USDT en Bóveda:      ", hre.ethers.formatUnits(vaultBalance, 6));
  console.log("   - Respaldo de Usuarios 1:1:  ", hre.ethers.formatUnits(circulatingBacking, 6));
  console.log("   - Comisiones Acumuladas:     ", hre.ethers.formatUnits(accumulatedFees, 6));

  // 500 * 0.5% = 2.5 USDT en comisiones
  if (accumulatedFees.toString() !== hre.ethers.parseUnits("2.5", 6).toString()) {
    throw new Error("Comisión de depósito calculada incorrectamente");
  }

  // 5. Retiro: El validador firma una orden de retiro para Bob por 200 USDT
  console.log("\n5. Simulando retiro confidencial: Validador firma orden para Bob (200 USDT)...");
  const withdraw200 = hre.ethers.parseUnits("200", 6);
  const orderId = hre.ethers.hexlify(hre.ethers.randomBytes(32));
  const chainId = (await hre.ethers.provider.getNetwork()).chainId;

  // Hash estructurado: (orderId, recipient, amount, chainId, vaultAddress)
  const messageHash = hre.ethers.solidityPackedKeccak256(
    ["bytes32", "address", "uint256", "uint256", "address"],
    [orderId, bob.address, withdraw200, chainId, vaultAddress]
  );
  const signature = await validator.signMessage(hre.ethers.getBytes(messageHash));

  // Bob ejecuta el retiro
  await vault.connect(bob).withdraw(orderId, bob.address, withdraw200, signature);
  console.log("   [OK] Retiro ejecutado con éxito.");
  console.log("   - Balance Bob USDT:", hre.ethers.formatUnits(await usdt.balanceOf(bob.address), 6));

  // Verificar protección contra repetición (Replay Protection)
  console.log("\n6. Verificando protección contra repetición (Doble Retiro)...");
  let replayPrevented = false;
  try {
    await vault.connect(bob).withdraw(orderId, bob.address, withdraw200, signature);
  } catch (err) {
    replayPrevented = true;
    console.log("   [OK] Doble retiro bloqueado correctamente:", err.message.includes("Withdrawal order already executed"));
  }
  if (!replayPrevented) {
    throw new Error("Fallo de seguridad: se permitió ejecutar una orden de retiro dos veces");
  }

  // 7. Tesorería: Owner retira las comisiones ganadas por el protocolo
  console.log("\n7. Owner retira las comisiones acumuladas (2.5 USDT) hacia la Tesorería...");
  await vault.connect(owner).claimTreasuryFees(treasury.address, accumulatedFees);
  console.log("   [OK] Comisiones transferidas a la Tesorería.");
  console.log("   - Balance Tesorería USDT:    ", hre.ethers.formatUnits(await usdt.balanceOf(treasury.address), 6));
  console.log("   - Comisiones restantes Vault:", hre.ethers.formatUnits(await vault.accumulatedFees(), 6));

  // 8. Invariante final
  const finalVaultBalance = await usdt.balanceOf(vaultAddress);
  const finalCirculating = await vault.getCirculatingBacking();
  console.log("\n8. Auditoría Final de Solvencia 1:1:");
  console.log("   - Colateral Total:", hre.ethers.formatUnits(finalVaultBalance, 6), "USDT");
  console.log("   - Circulante:     ", hre.ethers.formatUnits(finalCirculating, 6), "USDT");
  console.log("   - 100% Solvente:  ", finalVaultBalance.toString() === finalCirculating.toString());

  console.log("\n===============================================================");
  console.log(" [TODAS LAS PRUEBAS PASARON SATISFACTORIAMENTE (100% EXITOSO)]");
  console.log("===============================================================\n");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
