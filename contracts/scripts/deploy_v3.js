const { ethers } = require("hardhat");
require("dotenv").config();

async function main() {
  const [deployer] = await ethers.getSigners();

  const usdt = process.env.ARBITRUM_USDT_ADDRESS;
  const validators = (process.env.V3_VALIDATOR_ADDRESSES || "")
    .split(",")
    .map((v) => v.trim())
    .filter(Boolean);
  const threshold = Number(process.env.V3_VALIDATOR_THRESHOLD || "0");
  const depositFeeBps = Number(process.env.DEPOSIT_FEE_BPS || "50");
  const withdrawFeeBps = Number(process.env.WITHDRAW_FEE_BPS || "50");
  const owner = process.env.V3_OWNER_ADDRESS || deployer.address;

  if (!usdt || !ethers.isAddress(usdt)) throw new Error("ARBITRUM_USDT_ADDRESS is required");
  if (validators.length < 3) throw new Error("V3_VALIDATOR_ADDRESSES must contain at least 3 addresses");
  if (!validators.every(ethers.isAddress)) throw new Error("V3_VALIDATOR_ADDRESSES contains an invalid address");
  if (threshold < 2 || threshold > validators.length || threshold * 2 <= validators.length) {
    throw new Error("V3_VALIDATOR_THRESHOLD must be a strict majority and at least 2");
  }
  if (!ethers.isAddress(owner)) throw new Error("V3_OWNER_ADDRESS is invalid");

  console.log("Deploying CryptoPegVaultV3");
  console.log("Deployer:", deployer.address);
  console.log("USDT:", usdt);
  console.log("Validators:", validators);
  console.log("Threshold:", threshold);
  console.log("Owner:", owner);

  const Vault = await ethers.getContractFactory("CryptoPegVaultV3");
  const vault = await Vault.deploy(
    usdt,
    validators,
    threshold,
    depositFeeBps,
    withdrawFeeBps,
    owner
  );
  await vault.waitForDeployment();

  console.log("CryptoPegVaultV3:", await vault.getAddress());
  console.log("IMPORTANT: Do not fund/migrate real USDT until independent cosigners and checkpoint finality are verified end-to-end.");
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
