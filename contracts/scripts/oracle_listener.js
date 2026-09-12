const { ethers } = require("ethers");
const path = require("path");
require("dotenv").config({ path: path.resolve(__dirname, "../../.env") });

const ENABLE_ORACLE = (process.env.ENABLE_ORACLE || "false").toLowerCase() === "true";
const ARBITRUM_RPC_URL = process.env.ARBITRUM_SEPOLIA_RPC_URL || process.env.ARBITRUM_RPC_URL || "https://sepolia-rollup.arbitrum.io/rpc";
const VAULT_ADDRESS = process.env.USDT_VAULT_ADDRESS || "0x0ddFB2b3095DFC50E15bCD37b6A3a786a4DCB3e0";
const NODE_DAEMON_URL = process.env.NODE_DAEMON_URL || "http://127.0.0.1:8080";
const POLL_INTERVAL_MS = parseInt(process.env.ORACLE_POLL_INTERVAL_MS || "5000");

// Minimal ABI para escuchar el evento DepositInitiated de CryptoPegVault
const VAULT_ABI = [
  "event DepositInitiated(address indexed depositor, uint256 grossAmount, uint256 netMinted, uint256 fee, bytes32 stealthPubView, bytes32 stealthPubSpend, uint256 timestamp)"
];

async function main() {
  console.log("=================================================================");
  console.log("   CRYPTOPEG USDT -- SERVICIO ORÁCULO DE ENLACE ARBITRUM L2      ");
  console.log("=================================================================");

  if (!ENABLE_ORACLE) {
    console.log("\n[ORACLE] Modo Oráculo DESACTIVADO (ENABLE_ORACLE=false).");
    console.log("[ORACLE] Este nodo opera en modo NODO REGULAR P2P (Privado y sin dependencias externas).");
    console.log("[ORACLE] Para activar el puente y procesar depósitos on-chain, configura:");
    console.log("         ENABLE_ORACLE=true  en tu archivo .env\n");
    return;
  }

  console.log("\n[ORACLE] Modo Oráculo ACTIVADO (ENABLE_ORACLE=true).");
  console.log(" -> RPC Arbitrum:      ", ARBITRUM_RPC_URL);
  console.log(" -> Contrato Vault:    ", VAULT_ADDRESS);
  console.log(" -> Daemon Local C++:  ", NODE_DAEMON_URL);
  console.log(" -> Frecuencia Polling:", POLL_INTERVAL_MS, "ms\n");

  // Esperar a que el daemon C++ local esté activo y listo
  console.log(`[ORACLE] Verificando conexión con el nodo C++ en ${NODE_DAEMON_URL}...`);
  for (let attempt = 1; attempt <= 20; attempt++) {
    try {
      const hRes = await fetch(`${NODE_DAEMON_URL}/api/v1/node/health`);
      if (hRes.ok) {
        console.log(`[ORACLE] [OK] Daemon local C++ conectado y respondiendo.`);
        break;
      }
    } catch (e) {
      if (attempt === 20) {
        console.warn(`[ORACLE] [AVISO] Daemon local aún no responde tras 20s. Continuando sondeo en segundo plano...`);
      }
      await new Promise(r => setTimeout(r, 1000));
    }
  }

  const provider = new ethers.JsonRpcProvider(ARBITRUM_RPC_URL);
  const vaultContract = new ethers.Contract(VAULT_ADDRESS, VAULT_ABI, provider);

  // Registro de hashes procesados en memoria para evitar llamadas duplicadas
  const processedTxHashes = new Set();

  let lastCheckedBlock;
  try {
    lastCheckedBlock = await provider.getBlockNumber();
    console.log(`[ORACLE] Conectado exitosamente. Monitoreando desde bloque #${lastCheckedBlock}...`);
  } catch (err) {
    console.error("[ORACLE] Error conectando al RPC de Arbitrum:", err.message);
    process.exit(1);
  }

  // Bucle de sondeo continuo
  setInterval(async () => {
    try {
      const currentBlock = await provider.getBlockNumber();
      if (currentBlock <= lastCheckedBlock) return;

      const fromBlock = lastCheckedBlock + 1;
      const toBlock = currentBlock;

      const events = await vaultContract.queryFilter("DepositInitiated", fromBlock, toBlock);

      for (const event of events) {
        const txHash = event.transactionHash;
        if (processedTxHashes.has(txHash)) continue;

        processedTxHashes.add(txHash);

        const grossUSDT = parseFloat(ethers.formatUnits(event.args.grossAmount, 6));
        const viewKeyHex = event.args.stealthPubView.replace("0x", "");
        const spendKeyHex = event.args.stealthPubSpend.replace("0x", "");

        console.log(`\n[ORACLE] -> Nuevo depósito detectado en Arbitrum Sepolia!`);
        console.log(`         Tx Hash:  ${txHash}`);
        console.log(`         Monto:    ${grossUSDT.toFixed(6)} USDT`);
        console.log(`         View Key: ${viewKeyHex.substring(0, 16)}...`);
        console.log(`         Spend Key:${spendKeyHex.substring(0, 16)}...`);

        // Notificar al nodo daemon C++ para que acuñe el UTXO furtivo y emita el bloque LMDB
        try {
          const res = await fetch(`${NODE_DAEMON_URL}/api/v1/vault/deposit`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              gross_usdt: grossUSDT,
              stealth_pub_view: viewKeyHex,
              stealth_pub_spend: spendKeyHex,
              tx_hash: txHash
            })
          });

          const data = await res.json();
          if (res.ok) {
            console.log(`[ORACLE] [OK] Acuñado en Blockchain Privada! Bloque #${data.block_height} | Net Minted: ${data.net_shielded_minted} | Fee: ${data.fee_to_pool}`);
          } else {
            console.error(`[ORACLE] [ERROR] Nodo rechazó el depósito:`, data.error);
          }
        } catch (fetchErr) {
          console.error(`[ORACLE] [ERROR] No se pudo conectar al daemon local (${NODE_DAEMON_URL}):`, fetchErr.message);
        }
      }

      lastCheckedBlock = toBlock;
    } catch (pollErr) {
      console.warn(`[ORACLE] Aviso en sondeo de eventos (${pollErr.message}), reintentando en siguiente ciclo...`);
    }
  }, POLL_INTERVAL_MS);
}

main().catch((err) => {
  console.error("[ORACLE CRITICAL ERROR]:", err);
  process.exit(1);
});
