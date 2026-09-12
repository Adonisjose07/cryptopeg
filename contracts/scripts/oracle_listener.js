const { ethers } = require("ethers");
const path = require("path");
const fs = require("fs");
require("dotenv").config({ path: path.resolve(__dirname, "../../.env") });

const ENABLE_ORACLE = (process.env.ENABLE_ORACLE || "false").toLowerCase() === "true";
const ARBITRUM_RPC_URL = process.env.ARBITRUM_SEPOLIA_RPC_URL || process.env.ARBITRUM_RPC_URL || "https://sepolia-rollup.arbitrum.io/rpc";
const VAULT_ADDRESS = process.env.USDT_VAULT_ADDRESS || "0x0ddFB2b3095DFC50E15bCD37b6A3a786a4DCB3e0";
const NODE_DAEMON_URL = process.env.NODE_DAEMON_URL || "http://127.0.0.1:8080";
const POLL_INTERVAL_MS = parseInt(process.env.ORACLE_POLL_INTERVAL_MS || "5000");
const STATE_FILE = process.env.RELAYER_STATE_FILE || path.resolve(__dirname, "../../data/relayer_state.json");

// ABI bidireccional para CryptoPegVault en Arbitrum L2
const VAULT_ABI = [
  "event DepositInitiated(address indexed depositor, uint256 grossAmount, uint256 netMinted, uint256 fee, bytes32 stealthPubView, bytes32 stealthPubSpend, uint256 timestamp)",
  "event WithdrawalExecuted(bytes32 indexed orderId, address indexed recipient, uint256 amount, uint256 timestamp)",
  "function executedWithdrawals(bytes32 orderId) external view returns (bool)",
  "function withdraw(bytes32 orderId, address recipient, uint256 amount, bytes calldata signature) external",
  "function validatorSigner() external view returns (address)",
  "function accumulatedFees() external view returns (uint256)"
];

const TESTNET_PRIVATE_KEY = process.env.TESTNET_PRIVATE_KEY || process.env.VALIDATOR_PRIVATE_KEY || "";

async function main() {
  console.log("=================================================================");
  console.log("   CRYPTOPEG USDT -- SERVICIO ORÁCULO DE ENLACE BIDIRECCIONAL    ");
  console.log("   (Depósitos Arbitrum -> C++ | Retiros C++ -> Arbitrum L2)      ");
  console.log("=================================================================");

  if (!ENABLE_ORACLE) {
    console.log("\n[ORACLE] Modo Oráculo DESACTIVADO (ENABLE_ORACLE=false).");
    console.log("[ORACLE] Este nodo opera en modo NODO REGULAR P2P (Privado y sin dependencias externas).");
    console.log("[ORACLE] Para activar el puente bidireccional, configura:");
    console.log("         ENABLE_ORACLE=true  en tu archivo .env\n");
    return;
  }

  console.log("\n[ORACLE] Modo Oráculo ACTIVADO (ENABLE_ORACLE=true).");
  console.log(" -> RPC Arbitrum:      ", ARBITRUM_RPC_URL);
  console.log(" -> Contrato Vault:    ", VAULT_ADDRESS);
  console.log(" -> Daemon Local C++:  ", NODE_DAEMON_URL);
  console.log(" -> Frecuencia Polling:", POLL_INTERVAL_MS, "ms");

  // Esperar a que el daemon C++ local esté activo y listo
  console.log(`[ORACLE] Verificando conexión con el nodo C++ en ${NODE_DAEMON_URL}...`);
  for (let attempt = 1; attempt <= 30; attempt++) {
    try {
      const hRes = await fetch(`${NODE_DAEMON_URL}/api/v1/node/health`);
      if (hRes.ok) {
        console.log(`[ORACLE] [OK] Daemon local C++ conectado y respondiendo.`);
        break;
      }
    } catch (e) {
      if (attempt === 30) {
        console.warn(`[ORACLE] [AVISO] Daemon local aún no responde tras 30s. Continuando sondeo en segundo plano...`);
      }
      await new Promise(r => setTimeout(r, 1000));
    }
  }

  const provider = new ethers.JsonRpcProvider(ARBITRUM_RPC_URL);
  const vaultReadOnly = new ethers.Contract(VAULT_ADDRESS, VAULT_ABI, provider);

  // Configurar billetera del validador para firmar y despachar retiros en L2
  let validatorWallet = null;
  let vaultWithSigner = null;

  if (TESTNET_PRIVATE_KEY) {
    try {
      const cleanKey = TESTNET_PRIVATE_KEY.trim();
      const privKey = cleanKey.startsWith("0x") ? cleanKey : `0x${cleanKey}`;
      validatorWallet = new ethers.Wallet(privKey, provider);
      vaultWithSigner = new ethers.Contract(VAULT_ADDRESS, VAULT_ABI, validatorWallet);
      console.log(`[ORACLE-RELAYER] Billetera Validador L2 activa: ${validatorWallet.address}`);
    } catch (err) {
      console.warn(`[ORACLE-RELAYER] Error inicializando clave de validador:`, err.message);
    }
  } else {
    console.warn(`[ORACLE-RELAYER] [AVISO] TESTNET_PRIVATE_KEY no configurada. El relayer de retiros no podrá firmar transacciones on-chain.`);
  }

  // Verificar validador registrado en el contrato inteligente
  try {
    const onChainSigner = await vaultReadOnly.validatorSigner();
    console.log(`[ORACLE-RELAYER] Validator Signer en Smart Contract: ${onChainSigner}`);
    if (validatorWallet && validatorWallet.address.toLowerCase() !== onChainSigner.toLowerCase()) {
      console.warn(`[ORACLE-RELAYER] [ADVERTENCIA] La dirección de la billetera local (${validatorWallet.address}) no coincide con validatorSigner (${onChainSigner}).`);
    } else if (validatorWallet) {
      console.log(`[ORACLE-RELAYER] [OK] Coincidencia 100% con validador on-chain autorizado.`);
    }
  } catch (err) {
    console.warn(`[ORACLE-RELAYER] No se pudo consultar validatorSigner on-chain:`, err.message);
  }

  // Carga y guardado de estado en disco para resiliencia ante reinicios
  function loadState() {
    try {
      if (fs.existsSync(STATE_FILE)) {
        const raw = fs.readFileSync(STATE_FILE, "utf8");
        const data = JSON.parse(raw);
        return {
          lastCheckedL2Block: Number(data.lastCheckedL2Block || 0),
          lastCheckedChainHeight: Number(data.lastCheckedChainHeight || 0),
          processedTxHashes: new Set(data.processedTxHashes || []),
          processedWithdrawalOrders: new Set(data.processedWithdrawalOrders || [])
        };
      }
    } catch (e) {
      console.warn(`[ORACLE] No se pudo cargar archivo de estado (${e.message}). Iniciando estado limpio.`);
    }
    return {
      lastCheckedL2Block: 0,
      lastCheckedChainHeight: 0,
      processedTxHashes: new Set(),
      processedWithdrawalOrders: new Set()
    };
  }

  function saveState(state) {
    try {
      const dir = path.dirname(STATE_FILE);
      if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
      }
      const data = {
        lastCheckedL2Block: state.lastCheckedL2Block,
        lastCheckedChainHeight: state.lastCheckedChainHeight,
        processedTxHashes: Array.from(state.processedTxHashes).slice(-5000),
        processedWithdrawalOrders: Array.from(state.processedWithdrawalOrders).slice(-5000),
        updatedAt: new Date().toISOString()
      };
      fs.writeFileSync(STATE_FILE, JSON.stringify(data, null, 2), "utf8");
    } catch (e) {
      console.error(`[ORACLE] Error guardando estado en ${STATE_FILE}:`, e.message);
    }
  }

  // Estados de sondeo inicializados desde persistencia
  const savedState = loadState();
  const processedTxHashes = savedState.processedTxHashes;
  const processedWithdrawalOrders = savedState.processedWithdrawalOrders;
  let lastCheckedL2Block = savedState.lastCheckedL2Block;
  let lastCheckedChainHeight = savedState.lastCheckedChainHeight;

  let isProcessingDeposits = false;
  let isProcessingWithdrawals = false;

  try {
    const currentL2 = await provider.getBlockNumber();
    if (lastCheckedL2Block === 0 || lastCheckedL2Block > currentL2) {
      lastCheckedL2Block = currentL2;
    }
    console.log(`[ORACLE] Conectado exitosamente.`);
    console.log(` -> Monitoreando depósitos desde bloque L2 #${lastCheckedL2Block}`);
    console.log(` -> Monitoreando retiros desde bloque de cadena #${lastCheckedChainHeight}\n`);
  } catch (err) {
    console.error("[ORACLE] Error conectando al RPC de Arbitrum:", err.message);
    process.exit(1);
  }

  // 1. Tarea: Sondeo de Depósitos L2 -> C++ Daemon
  async function pollInboundDeposits() {
    if (isProcessingDeposits) return;
    isProcessingDeposits = true;

    try {
      const currentBlock = await provider.getBlockNumber();
      if (currentBlock <= lastCheckedL2Block) return;

      const fromBlock = lastCheckedL2Block + 1;
      const toBlock = currentBlock;

      const events = await vaultContractFilter(fromBlock, toBlock);

      for (const event of events) {
        const txHash = event.transactionHash;
        if (processedTxHashes.has(txHash)) continue;

        processedTxHashes.add(txHash);

        const grossUSDT = parseFloat(ethers.formatUnits(event.args.grossAmount, 6));
        const viewKeyHex = event.args.stealthPubView.replace("0x", "");
        const spendKeyHex = event.args.stealthPubSpend.replace("0x", "");

        console.log(`\n[ORACLE-INBOUND] -> Nuevo depósito detectado en Arbitrum Sepolia!`);
        console.log(`                 Tx Hash:  ${txHash}`);
        console.log(`                 Monto:    ${grossUSDT.toFixed(6)} USDT`);
        console.log(`                 View Key: ${viewKeyHex.substring(0, 16)}...`);
        console.log(`                 Spend Key:${spendKeyHex.substring(0, 16)}...`);

        try {
          const res = await fetch(`${NODE_DAEMON_URL}/api/v1/vault/deposit`, {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
              "X-Oracle-Secret": process.env.ORACLE_SECRET || "cryptopeg_oracle_secret_2026"
            },
            body: JSON.stringify({
              gross_usdt: grossUSDT,
              stealth_pub_view: viewKeyHex,
              stealth_pub_spend: spendKeyHex,
              tx_hash: txHash,
              timestamp: Number(event.args.timestamp)
            })
          });

          const data = await res.json();
          if (res.ok) {
            console.log(`[ORACLE-INBOUND] [OK] Acuñado en Blockchain Privada! Bloque #${data.block_height} | Net Minted: ${data.net_shielded_minted} | Fee: ${data.fee_to_pool}`);
          } else {
            console.error(`[ORACLE-INBOUND] [ERROR] Nodo rechazó el depósito:`, data.error);
          }
        } catch (fetchErr) {
          console.error(`[ORACLE-INBOUND] [ERROR] No se pudo conectar al daemon local (${NODE_DAEMON_URL}):`, fetchErr.message);
        }
      }

      lastCheckedL2Block = toBlock;
      saveState({ lastCheckedL2Block, lastCheckedChainHeight, processedTxHashes, processedWithdrawalOrders });
    } catch (pollErr) {
      console.warn(`[ORACLE-INBOUND] Aviso en sondeo de eventos (${pollErr.message}), reintentando...`);
    } finally {
      isProcessingDeposits = false;
    }
  }

  async function vaultContractFilter(fromBlock, toBlock) {
    try {
      return await vaultReadOnly.queryFilter("DepositInitiated", fromBlock, toBlock);
    } catch (e) {
      return [];
    }
  }

  // 2. Tarea: Sondeo y Relayer de Retiros C++ Daemon -> Arbitrum L2
  async function pollOutboundWithdrawals() {
    if (!vaultWithSigner || !validatorWallet) return;
    if (isProcessingWithdrawals) return;
    isProcessingWithdrawals = true;

    try {
      const statusRes = await fetch(`${NODE_DAEMON_URL}/api/v1/node/status`);
      if (!statusRes.ok) return;
      const status = await statusRes.json();
      const currentHeight = Number(status.blockchain_height || 0);
      if (currentHeight <= 0) return;

      const startHeight = lastCheckedChainHeight === 0 ? 1 : lastCheckedChainHeight + 1;
      if (startHeight > currentHeight) return;

      for (let h = startHeight; h <= currentHeight; h++) {
        let blockSucceeded = true;
        try {
          const blockRes = await fetch(`${NODE_DAEMON_URL}/api/v1/chain/block/${h}`);
          if (!blockRes.ok) {
            blockSucceeded = false;
            break;
          }
          const block = await blockRes.json();

          if (block.withdrawals && block.withdrawals.length > 0) {
            for (const w of block.withdrawals) {
              const orderIdStr = w.order_id;
              if (!orderIdStr || processedWithdrawalOrders.has(orderIdStr)) {
                continue;
              }

              const orderIdHash = ethers.keccak256(ethers.toUtf8Bytes(orderIdStr));

              // Verificar si ya fue ejecutada en el Smart Contract on-chain
              const isExecuted = await vaultReadOnly.executedWithdrawals(orderIdHash);
              if (isExecuted) {
                processedWithdrawalOrders.add(orderIdStr);
                continue;
              }

              const recipient = w.destination;
              if (!recipient || !ethers.isAddress(recipient)) {
                console.warn(`[ORACLE-RELAYER] Bloque #${h} - Orden ${orderIdStr} tiene dirección de destino no válida en Arbitrum: "${recipient}". Omitiendo.`);
                processedWithdrawalOrders.add(orderIdStr);
                continue;
              }

              const amount = BigInt(w.net_amount_raw || Math.round(parseFloat(w.net_tumbled) * 1e6));
              if (amount <= 0n) {
                console.warn(`[ORACLE-RELAYER] Bloque #${h} - Orden ${orderIdStr} tiene monto neto 0.`);
                processedWithdrawalOrders.add(orderIdStr);
                continue;
              }

              console.log(`\n=================================================================`);
              console.log(`[ORACLE-RELAYER] -> NUEVA SOLICITUD DE RETIRO DETECTADA EN CADENA`);
              console.log(`=================================================================`);
              console.log(` -> Bloque Privado:     #${h}`);
              console.log(` -> Orden ID:           ${orderIdStr}`);
              console.log(` -> Destino Arbitrum:   ${recipient}`);
              console.log(` -> Monto Neto a Enviar:${ethers.formatUnits(amount, 6)} USDT`);

              const network = await provider.getNetwork();
              const chainId = network.chainId;

              // keccak256(abi.encodePacked(orderId, recipient, amount, block.chainid, address(this)))
              const messageHash = ethers.solidityPackedKeccak256(
                ["bytes32", "address", "uint256", "uint256", "address"],
                [orderIdHash, recipient, amount, chainId, VAULT_ADDRESS]
              );

              const signature = await validatorWallet.signMessage(ethers.getBytes(messageHash));
              console.log(`[ORACLE-RELAYER] Firma ECDSA generada por Validador ${validatorWallet.address}.`);
              console.log(`[ORACLE-RELAYER] Transmitiendo CryptoPegVault.withdraw(...) a Arbitrum Sepolia...`);

              try {
                const tx = await vaultWithSigner.withdraw(orderIdHash, recipient, amount, signature);
                console.log(`[ORACLE-RELAYER] Tx enviada a Arbitrum Sepolia! Hash: ${tx.hash}`);
                console.log(`[ORACLE-RELAYER] Esperando confirmación del bloque L2...`);

                const receipt = await tx.wait();
                console.log(`[ORACLE-RELAYER] [EXITO TOTAL] Retiro confirmado en bloque #${receipt.blockNumber}! Gas: ${receipt.gasUsed}`);
                console.log(`[ORACLE-RELAYER] Enlace Arbiscan: https://sepolia.arbiscan.io/tx/${tx.hash}\n`);
                processedWithdrawalOrders.add(orderIdStr);
              } catch (txErr) {
                if (txErr.message && txErr.message.includes("Withdrawal order already executed")) {
                  console.log(`[ORACLE-RELAYER] Orden ${orderIdStr} ya fue ejecutada on-chain.`);
                  processedWithdrawalOrders.add(orderIdStr);
                } else {
                  console.error(`[ORACLE-RELAYER] [ERROR] Fallo al ejecutar withdraw en Arbitrum:`, txErr.message);
                  blockSucceeded = false;
                  break;
                }
              }
            }
          }
        } catch (blockErr) {
          console.warn(`[ORACLE-RELAYER] Error procesando bloque #${h}:`, blockErr.message);
          blockSucceeded = false;
        }

        if (!blockSucceeded) {
          console.warn(`[ORACLE-RELAYER] Deteniendo avance en bloque #${h} debido a un fallo. Se reintentará en el siguiente ciclo.`);
          break;
        }

        lastCheckedChainHeight = h;
        saveState({ lastCheckedL2Block, lastCheckedChainHeight, processedTxHashes, processedWithdrawalOrders });
      }
    } catch (err) {
      console.warn(`[ORACLE-RELAYER] Aviso en sondeo de retiros (${err.message}).`);
    } finally {
      isProcessingWithdrawals = false;
    }
  }

  // Bucle de sondeo continuo
  setInterval(async () => {
    await Promise.allSettled([
      pollInboundDeposits(),
      pollOutboundWithdrawals()
    ]);
  }, POLL_INTERVAL_MS);
}

main().catch((err) => {
  console.error("[ORACLE CRITICAL ERROR]:", err);
  process.exit(1);
});
