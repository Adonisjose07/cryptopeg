const hre = require("hardhat");

async function main() {
  const [deployer] = await hre.ethers.getSigners();
  console.log("===============================================================");
  console.log(" Desplegando Contratos de Custodia CryptoPeg en:", hre.network.name);
  console.log(" Cuenta Desplegadora:", deployer.address);
  console.log(" Balance de Gas:", (await hre.ethers.provider.getBalance(deployer.address)).toString());
  console.log("===============================================================\n");

  let usdtAddress = process.env.ARBITRUM_USDT_ADDRESS;

  // Si estamos en Sepolia (L1 o L2) o red local y no se especificó un USDT, desplegamos MockUSDT
  if (!usdtAddress || hre.network.name === "arbitrumSepolia" || hre.network.name === "sepolia" || hre.network.name === "hardhat") {
    console.log("1. Desplegando MockUSDT (Tether USD 6 decimales)...");
    const MockUSDT = await hre.ethers.getContractFactory("MockUSDT");
    const mockUsdt = await MockUSDT.deploy();
    await mockUsdt.waitForDeployment();
    usdtAddress = await mockUsdt.getAddress();
    console.log("   [OK] MockUSDT desplegado en:", usdtAddress);
  } else {
    console.log("1. Usando USDT existente en:", usdtAddress);
  }

  // Dirección del validador / oráculo del nodo que firmará los retiros quemados
  const validatorSigner = process.env.VALIDATOR_SIGNER_ADDRESS || deployer.address;
  const depositFeeBps = parseInt(process.env.DEPOSIT_FEE_BPS || "50");   // 0.50%
  const withdrawFeeBps = parseInt(process.env.WITHDRAW_FEE_BPS || "50"); // 0.50%
  const initialOwner = process.env.OWNER_ADDRESS || deployer.address;

  console.log("\n2. Desplegando CryptoPegVault...");
  console.log("   - Token USDT:       ", usdtAddress);
  console.log("   - Validador Signer: ", validatorSigner);
  console.log("   - Fee Depósito:     ", depositFeeBps, "bps (0.50%)");
  console.log("   - Fee Retiro:       ", withdrawFeeBps, "bps (0.50%)");
  console.log("   - Dueño Inicial:    ", initialOwner);

  const CryptoPegVault = await hre.ethers.getContractFactory("CryptoPegVault");
  const vault = await CryptoPegVault.deploy(
    usdtAddress,
    validatorSigner,
    depositFeeBps,
    withdrawFeeBps,
    initialOwner
  );
  await vault.waitForDeployment();
  const vaultAddress = await vault.getAddress();

  console.log("\n===============================================================");
  console.log(" [EXITO] Despliegue completado con éxito:");
  console.log(" USDT_TOKEN_ADDRESS:  ", usdtAddress);
  console.log(" USDT_VAULT_ADDRESS:  ", vaultAddress);
  console.log("===============================================================");
  console.log("\nActualiza tus variables en el archivo .env con:");
  console.log(`USDT_VAULT_ADDRESS=${vaultAddress}`);
  console.log(`ARBITRUM_USDT_ADDRESS=${usdtAddress}`);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
