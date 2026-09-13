const hre = require("hardhat");
const fs = require("fs");
const path = require("path");

async function main() {
  console.log("=================================================================");
  console.log("  DESPLIEGUE CRYPTOPEG VAULT V2 EN ARBITRUM (EIP-712 COMPLIANT)  ");
  console.log("=================================================================");

  const [deployer] = await hre.ethers.getSigners();
  console.log("Desplegando con la cuenta:", deployer.address);
  const balance = await hre.ethers.provider.getBalance(deployer.address);
  console.log("Saldo cuenta:", hre.ethers.formatEther(balance), "ETH");

  const USDT_ADDRESS = process.env.ARBITRUM_USDT_ADDRESS || "0x900A96C51aac4EB8aF5FDa39bc0Ef13ADBe88B44";
  const VALIDATOR_ADDRESS = process.env.TREASURY_WALLET_ADDRESS || deployer.address;
  const DEPOSIT_FEE_BPS = 50;  // 0.50%
  const WITHDRAW_FEE_BPS = 50; // 0.50%

  console.log("\nParámetros de despliegue V2:");
  console.log(" -> Token USDT Colateral: ", USDT_ADDRESS);
  console.log(" -> Validador Firmante  : ", VALIDATOR_ADDRESS);
  console.log(" -> Comisión Depósito   : ", DEPOSIT_FEE_BPS, "bps (0.5%)");
  console.log(" -> Comisión Retiro     : ", WITHDRAW_FEE_BPS, "bps (0.5%)");
  console.log(" -> Propietario Inicial : ", deployer.address);

  const CryptoPegVaultV2 = await hre.ethers.getContractFactory("CryptoPegVaultV2");
  const vaultV2 = await CryptoPegVaultV2.deploy(
    USDT_ADDRESS,
    VALIDATOR_ADDRESS,
    DEPOSIT_FEE_BPS,
    WITHDRAW_FEE_BPS,
    deployer.address
  );

  await vaultV2.waitForDeployment();
  const vaultV2Address = await vaultV2.getAddress();
  console.log("\n[EXITO] CryptoPegVaultV2 desplegado exitosamente en:", vaultV2Address);

  // Guardar metadata de despliegue
  const deployDir = path.resolve(__dirname, "../deployments");
  if (!fs.existsSync(deployDir)) {
    fs.mkdirSync(deployDir, { recursive: true });
  }

  const networkName = hre.network.name;
  const deployData = {
    network: networkName,
    chainId: (await hre.ethers.provider.getNetwork()).chainId.toString(),
    contracts: {
      USDT: USDT_ADDRESS,
      CryptoPegVaultV2: vaultV2Address
    },
    validatorSigner: VALIDATOR_ADDRESS,
    depositFeeBps: DEPOSIT_FEE_BPS,
    withdrawFeeBps: WITHDRAW_FEE_BPS,
    deployedAt: new Date().toISOString()
  };

  fs.writeFileSync(
    path.join(deployDir, `${networkName}_v2.json`),
    JSON.stringify(deployData, null, 2)
  );
  console.log(`[INFO] Registro de despliegue guardado en deployments/${networkName}_v2.json`);
}

main().catch((error) => {
  console.error("[ERROR EN DESPLIEGUE V2]:", error);
  process.exitCode = 1;
});
