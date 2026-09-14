const crypto = require("crypto");
const { ethers } = require("ethers");
const path = require("path");
const fs = require("fs");
require("dotenv").config({ path: path.resolve(__dirname, "../../.env") });

const ENABLE_ORACLE = (process.env.ENABLE_ORACLE || "false").toLowerCase() === "true";
const ARBITRUM_RPC_URL = process.env.ARBITRUM_SEPOLIA_RPC_URL || process.env.ARBITRUM_RPC_URL || "https://sepolia-rollup.arbitrum.io/rpc";
const VAULT_ADDRESS = process.env.USDT_VAULT_ADDRESS || "0x511A31987EF1019a41CBba658935515Dd64d2D18";
const NODE_DAEMON_URL = process.env.NODE_DAEMON_URL || "http://127.0.0.1:8080";
const POLL_INTERVAL_MS = parseInt(process.env.ORACLE_POLL_INTERVAL_MS || "5000");
const L2_CONFIRMATION_BLOCKS = parseInt(process.env.L2_CONFIRMATION_BLOCKS || "12");
const CONFIDENTIAL_CHAIN_CONFIRMATIONS = parseInt(process.env.CONFIDENTIAL_CHAIN_CONFIRMATIONS || "6");
const STATE_FILE = process.env.RELAYER_STATE_FILE || path.resolve(__dirname, "../../data/relayer_state.json");

// Helper para derivar par de claves Ed25519 para atestaciones criptográficas de depósito (Auditoría v3 - P0-01)
function getEd25519KeyPair() {
  const seedHex = process.env.VALIDATOR_ED25519_PRIVKEY || process.env.VALIDATOR_PRIVATE_KEY || process.env.TESTNET_PRIVATE_KEY || "";
  if (!seedHex) {
    throw new Error("[ORACLE SECURITY CRITICAL] Clave de validador no configurada en VALIDATOR_ED25519_PRIVKEY o VALIDATOR_PRIVATE_KEY. Abortando por seguridad (V4-08).");
  }
  let seed;
  if (seedHex.replace(/^0x/, "").length === 64) {
    seed = Buffer.from(seedHex.replace(/^0x/, ""), "hex");
  } else {
    seed = crypto.createHash("sha256").update(seedHex).digest();
  }
  const privKey = crypto.createPrivateKey({
    key: Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]),
    format: "der",
    type: "pkcs8"
  });
  const pubKey = crypto.createPublicKey(privKey);
  const rawPubHex = pubKey.export({ type: "spki", format: "der" }).subarray(-32).toString("hex");
  return { privKey, pubKey, rawPubHex };
}

function computeDepositAttestationHash(chainId, contractAddress, txHash, logIndex, grossRawStr, viewKeyHex, spendKeyHex, l2BlockNumber) {
  const hash = crypto.createHash("sha256");
  hash.update(Buffer.from("DEPOSIT_ATTESTATION_V1", "utf8"));
  const bufChain = Buffer.alloc(8);
  bufChain.writeBigUInt64LE(BigInt(chainId));
  hash.update(bufChain);
  hash.update(Buffer.from(contractAddress, "utf8"));
  hash.update(Buffer.from(txHash, "utf8"));
  const bufLog = Buffer.alloc(4);
  bufLog.writeUInt32LE(Number(logIndex));
  hash.update(bufLog);
  const bufGross = Buffer.alloc(8);
  bufGross.writeBigUInt64LE(BigInt(grossRawStr));
  hash.update(bufGross);
  hash.update(Buffer.from(viewKeyHex, "hex"));
  hash.update(Buffer.from(spendKeyHex, "hex"));
  const bufBlock = Buffer.alloc(8);
  bufBlock.writeBigUInt64LE(BigInt(l2BlockNumber));
  hash.update(bufBlock);
  return hash.digest();
}

// ABI bidireccional para CryptoPegVault (compatible con V1 y V2 EIP-712)
const VAULT_ABI = [
  "event DepositInitiated(address indexed depositor, uint256 grossAmount, uint256 netMinted, uint256 fee, bytes32 stealthPubView, bytes32 stealthPubSpend, uint256 timestamp)",
  "event WithdrawalExecuted(bytes32 indexed orderId, address indexed recipient, uint256 amount, uint256 timestamp)",
  "event WithdrawalExecuted(bytes32 indexed orderId, address indexed recipient, uint256 amount, uint256 fee, uint256 timestamp)",
  "function executedWithdrawals(bytes32 orderId) external view returns (bool)",
  "function withdraw(bytes32 orderId, address recipient, uint256 amount, bytes calldata signature) external",
  "function withdraw(bytes32 orderId, address recipient, uint256 amount, uint256 fee, bytes calldata signature) external",
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
          processedWithdrawalOrders: new Set(data.processedWithdrawalOrders || []),
          quarantinedWithdrawalOrders: new Set(data.quarantinedWithdrawalOrders || [])
        };
      }
    } catch (e) {
      console.warn(`[ORACLE] No se pudo cargar archivo de estado (${e.message}). Iniciando estado limpio.`);
    }
    return {
      lastCheckedL2Block: 0,
      lastCheckedChainHeight: 0,
      processedTxHashes: new Set(),
      processedWithdrawalOrders: new Set(),
      quarantinedWithdrawalOrders: new Set()
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
        quarantinedWithdrawalOrders: Array.from(state.quarantinedWithdrawalOrders || []).slice(-5000),
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
  const quarantinedWithdrawalOrders = savedState.quarantinedWithdrawalOrders;
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
      const confirmedBlock = Math.max(0, currentBlock - L2_CONFIRMATION_BLOCKS);
      if (confirmedBlock <= lastCheckedL2Block) return;

      const fromBlock = lastCheckedL2Block + 1;
      const toBlock = confirmedBlock;

      let events;
      try {
        events = await vaultContractFilter(fromBlock, toBlock);
      } catch (filterErr) {
        console.error(`[ORACLE-INBOUND] Error consultando eventos L2 en rango [${fromBlock}-${toBlock}]:`, filterErr.message);
        // Salir inmediatamente sin alterar lastCheckedL2Block para no omitir depósitos ante fallas RPC (AUD-HIGH-01)
        return;
      }

      // Orden canónico determinista estricto (AUD-H0-P1-02)
      // Garantiza que todos los oráculos independientes procesen los eventos en el mismo orden exacto
      events.sort((a, b) => {
        if (a.blockNumber !== b.blockNumber) return a.blockNumber - b.blockNumber;
        if (a.transactionIndex !== b.transactionIndex) return a.transactionIndex - b.transactionIndex;
        const indexA = a.index !== undefined ? a.index : (a.logIndex !== undefined ? a.logIndex : 0);
        const indexB = b.index !== undefined ? b.index : (b.logIndex !== undefined ? b.logIndex : 0);
        return indexA - indexB;
      });

      const network = await provider.getNetwork();
      const chainId = Number(network.chainId);

      for (const event of events) {
        const txHash = event.transactionHash;
        const logIndex = event.index !== undefined ? event.index : (event.logIndex !== undefined ? event.logIndex : 0);
        const eventId = `${chainId}:${VAULT_ADDRESS.toLowerCase()}:${txHash}:${logIndex}`;
        if (processedTxHashes.has(eventId) || processedTxHashes.has(txHash)) continue;

        const grossRawStr = event.args.grossAmount.toString();
        const grossUSDT = parseFloat(ethers.formatUnits(event.args.grossAmount, 6));
        const viewKeyHex = event.args.stealthPubView.replace("0x", "");
        const spendKeyHex = event.args.stealthPubSpend.replace("0x", "");

        console.log(`\n[ORACLE-INBOUND] -> Nuevo depósito confirmado en Arbitrum!`);
        console.log(`                 Tx Hash:  ${txHash}`);
        console.log(`                 Log Index:${logIndex}`);
        console.log(`                 Monto:    ${grossUSDT.toFixed(6)} USDT (${grossRawStr} micro-USDT)`);
        console.log(`                 View Key: ${viewKeyHex.substring(0, 16)}...`);
        console.log(`                 Spend Key:${spendKeyHex.substring(0, 16)}...`);

        // Generar atestación criptográfica Ed25519 (Auditoría v3 - P0-01)
        const { privKey, rawPubHex } = getEd25519KeyPair();
        const attestationHash = computeDepositAttestationHash(
          chainId, VAULT_ADDRESS, txHash, logIndex, grossRawStr, viewKeyHex, spendKeyHex, event.blockNumber
        );
        const signature = crypto.sign(null, attestationHash, privKey);
        const sigHex = signature.toString("hex");

        let processedSuccessfully = false;
        try {
          const res = await fetch(`${NODE_DAEMON_URL}/api/v1/vault/deposit`, {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            },
            body: JSON.stringify({
              chain_id: chainId,
              contract_address: VAULT_ADDRESS,
              tx_hash: txHash,
              log_index: logIndex,
              event_id: eventId,
              gross_usdt: grossUSDT,
              gross_usdt_raw: grossRawStr,
              stealth_pub_view: viewKeyHex,
              stealth_pub_spend: spendKeyHex,
              l2_block_number: event.blockNumber,
              timestamp: Number(event.args.timestamp),
              validator_pubkey: rawPubHex,
              validator_signature: sigHex
            })
          });

          const data = await res.json();
          if (res.ok || res.status === 409) {
            processedTxHashes.add(eventId);
            processedTxHashes.add(txHash);
            processedSuccessfully = true;
            if (res.ok) {
              console.log(`[ORACLE-INBOUND] [OK] Acuñado en Blockchain Privada! Bloque #${data.block_height} | Net Minted: ${data.net_shielded_minted} | Fee: ${data.fee_to_pool}`);
            } else {
              console.log(`[ORACLE-INBOUND] [INFO] Depósito ya procesado previamente en el nodo: ${txHash}`);
            }
          } else {
            console.error(`[ORACLE-INBOUND] [ERROR] Nodo rechazó el depósito (${res.status}):`, data.error);
          }
        } catch (fetchErr) {
          console.error(`[ORACLE-INBOUND] [ERROR] No se pudo conectar al daemon local (${NODE_DAEMON_URL}):`, fetchErr.message);
        }

        if (!processedSuccessfully) {
          // Si falló por desconexión o error transitorio, retroceder cursor para reintentar en el próximo sondeo (AUD-H0-06)
          lastCheckedL2Block = event.blockNumber > 0 ? event.blockNumber - 1 : lastCheckedL2Block;
          saveState({ lastCheckedL2Block, lastCheckedChainHeight, processedTxHashes, processedWithdrawalOrders, quarantinedWithdrawalOrders });
          return;
        }
      }

      lastCheckedL2Block = toBlock;
      saveState({ lastCheckedL2Block, lastCheckedChainHeight, processedTxHashes, processedWithdrawalOrders, quarantinedWithdrawalOrders });
    } catch (pollErr) {
      console.warn(`[ORACLE-INBOUND] Aviso en sondeo de eventos (${pollErr.message}), reintentando...`);
    } finally {
      isProcessingDeposits = false;
    }
  }

  async function vaultContractFilter(fromBlock, toBlock) {
    return await vaultReadOnly.queryFilter("DepositInitiated", fromBlock, toBlock);
  }

  // Mapa para rastrear reintentos fallidos y prevenir Head-of-Line Blocking (V4-05)
  const orderFailures = new Map();

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

      // Profundidad de confirmaciones requeridas en la cadena confidencial para evitar doble gasto por reorgs (CP-AUD-02)
      const maxSafeHeight = Math.max(0, currentHeight - CONFIDENTIAL_CHAIN_CONFIRMATIONS);
      if (maxSafeHeight <= 0) return;

      const startHeight = lastCheckedChainHeight === 0 ? 1 : lastCheckedChainHeight + 1;
      if (startHeight > maxSafeHeight) return;

      for (let h = startHeight; h <= maxSafeHeight; h++) {
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
              if (!orderIdStr || processedWithdrawalOrders.has(orderIdStr) || quarantinedWithdrawalOrders.has(orderIdStr)) {
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
                console.warn(`[ORACLE-RELAYER] Bloque #${h} - Orden ${orderIdStr} tiene dirección de destino no válida en Arbitrum: "${recipient}". Aislándola en DLQ.`);
                quarantinedWithdrawalOrders.add(orderIdStr);
                continue;
              }

              const amount = BigInt(w.net_amount_raw || Math.round(parseFloat(w.net_tumbled) * 1e6));
              if (amount <= 0n) {
                console.warn(`[ORACLE-RELAYER] Bloque #${h} - Orden ${orderIdStr} tiene monto neto 0. Aislándola en DLQ.`);
                quarantinedWithdrawalOrders.add(orderIdStr);
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

              // 1. Firma tipada estructurada EIP-712 (AUD-INFO-02)
              const domain = {
                name: "CryptoPegVault",
                version: "2",
                chainId: chainId,
                verifyingContract: VAULT_ADDRESS
              };

              const types = {
                Withdrawal: [
                  { name: "orderId", type: "bytes32" },
                  { name: "recipient", type: "address" },
                  { name: "amount", type: "uint256" },
                  { name: "fee", type: "uint256" }
                ]
              };

              const withdrawFee = BigInt(w.fee_raw || 0);

              const value = {
                orderId: orderIdHash,
                recipient: recipient,
                amount: amount,
                fee: withdrawFee
              };

              let signature;
              try {
                signature = await validatorWallet.signTypedData(domain, types, value);
                console.log(`[ORACLE-RELAYER] Firma tipada EIP-712 generada por Validador ${validatorWallet.address}.`);
              } catch (signErr) {
                const messageHash = ethers.solidityPackedKeccak256(
                  ["bytes32", "address", "uint256", "uint256", "address"],
                  [orderIdHash, recipient, amount, chainId, VAULT_ADDRESS]
                );
                signature = await validatorWallet.signMessage(ethers.getBytes(messageHash));
                console.log(`[ORACLE-RELAYER] Firma legada eth_sign generada por Validador ${validatorWallet.address}.`);
              }

              console.log(`[ORACLE-RELAYER] Transmitiendo withdraw(...) a Arbitrum Sepolia...`);

              try {
                let tx;
                // Si el contrato posee la función V2 con comisiones y EIP-712
                if (typeof vaultWithSigner["withdraw(bytes32,address,uint256,uint256,bytes)"] === "function") {
                  tx = await vaultWithSigner["withdraw(bytes32,address,uint256,uint256,bytes)"](
                    orderIdHash, recipient, amount, withdrawFee, signature
                  );
                } else {
                  // Fallback para contrato V1 legado sin comisiones (4 parámetros)
                  const legacyHash = ethers.solidityPackedKeccak256(
                    ["bytes32", "address", "uint256", "uint256", "address"],
                    [orderIdHash, recipient, amount, chainId, VAULT_ADDRESS]
                  );
                  const legacySig = await validatorWallet.signMessage(ethers.getBytes(legacyHash));
                  tx = await vaultWithSigner["withdraw(bytes32,address,uint256,bytes)"](
                    orderIdHash, recipient, amount, legacySig
                  );
                }

                console.log(`[ORACLE-RELAYER] Tx enviada a Arbitrum Sepolia! Hash: ${tx.hash}`);
                console.log(`[ORACLE-RELAYER] Esperando confirmación del bloque L2...`);

                const receipt = await tx.wait();
                console.log(`[ORACLE-RELAYER] [EXITO TOTAL] Retiro confirmado en bloque #${receipt.blockNumber}! Gas: ${receipt.gasUsed}`);
                console.log(`[ORACLE-RELAYER] Enlace Arbiscan: https://sepolia.arbiscan.io/tx/${tx.hash}\n`);
                processedWithdrawalOrders.add(orderIdStr);
              } catch (txErr) {
                const errMsg = txErr.message || "";

                // Verificar directamente en el Smart Contract si la orden ya fue ejecutada (CP-AUD-03)
                let alreadyExecutedOnChain = false;
                try {
                  alreadyExecutedOnChain = await vaultReadOnly.executedWithdrawals(orderIdHash);
                } catch (_) {}

                if (alreadyExecutedOnChain || errMsg.includes("Withdrawal order already executed")) {
                  console.warn(`[ORACLE-RELAYER] Orden ${orderIdStr} ya confirmada/ejecutada on-chain. Marcando como procesada.`);
                  processedWithdrawalOrders.add(orderIdStr);
                } else {
                  // Fallos de transacción, reversión por pausa, saldo insuficiente o error transitorio:
                  // NUNCA descartar agregando a processedWithdrawalOrders (CP-AUD-03)
                  const fails = (orderFailures.get(orderIdStr) || 0) + 1;
                  orderFailures.set(orderIdStr, fails);
                  if (fails >= 3) {
                    console.error(`[ORACLE-RELAYER] [CRÍTICO - CUARENTENA DLQ] Orden ${orderIdStr} aislada y colocada en CUARENTENA tras 3 intentos fallidos (${errMsg}). Desbloqueando cola sin marcar como procesada para no descartar fondos.`);
                    quarantinedWithdrawalOrders.add(orderIdStr);
                  } else {
                    console.error(`[ORACLE-RELAYER] [REINTENTO ${fails}/3] Fallo al ejecutar withdraw en Arbitrum (${errMsg}). Reintentando en siguiente sondeo.`);
                    blockSucceeded = false;
                    break;
                  }
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
        saveState({ lastCheckedL2Block, lastCheckedChainHeight, processedTxHashes, processedWithdrawalOrders, quarantinedWithdrawalOrders });
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
