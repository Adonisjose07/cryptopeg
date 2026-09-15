const fs = require("fs");
const path = require("path");
const { ethers } = require("ethers");
require("dotenv").config();

const RPC_URL = process.env.ARBITRUM_SEPOLIA_RPC_URL || process.env.ARBITRUM_RPC_URL;
const VAULT_ADDRESS = process.env.BRIDGE_V3_VAULT_ADDRESS || "";
const NODE_URL = (process.env.NODE_DAEMON_URL || "http://127.0.0.1:8080").replace(/\/$/, "");
const RELAYER_PRIVATE_KEY = process.env.V3_RELAYER_PRIVATE_KEY || "";
const COSIGNERS = JSON.parse(process.env.V3_COSIGNERS_JSON || "[]");
const POLL_MS = Number(process.env.V3_RELAYER_POLL_MS || "5000");
const CONFIRMATIONS = Number(process.env.CONFIDENTIAL_CHAIN_CONFIRMATIONS || "6");
const STATE_FILE = process.env.V3_RELAYER_STATE_FILE || path.resolve(__dirname, "../../data/v3_relayer_state.json");

const ABI = [
  "function validatorThreshold() view returns (uint256)",
  "function latestFinalizedHeight() view returns (uint64)",
  "function latestFinalizedBlockHash() view returns (bytes32)",
  "function executedWithdrawals(bytes32) view returns (bool)",
  "function finalizeCheckpoint(uint64,bytes32,bytes32,bytes[]) external",
  "function withdraw(bytes32,address,uint256,uint256,uint64,bytes32,uint64,bytes32,bytes[]) external"
];

function fail(message) { throw new Error(message); }

async function getJson(url) {
  const res = await fetch(url);
  if (!res.ok) fail(`HTTP ${res.status} from ${url}`);
  return res.json();
}

async function requestSignature(cosigner, endpoint, payload) {
  const res = await fetch(`${String(cosigner.url).replace(/\/$/, "")}${endpoint}`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "authorization": `Bearer ${cosigner.token}`,
    },
    body: JSON.stringify(payload),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) fail(`${cosigner.url}: ${data.error || `HTTP ${res.status}`}`);
  return data;
}

function loadState() {
  try { return JSON.parse(fs.readFileSync(STATE_FILE, "utf8")); }
  catch (_) { return { lastWithdrawalHeight: 0 }; }
}

function saveState(state) {
  fs.mkdirSync(path.dirname(STATE_FILE), { recursive: true });
  fs.writeFileSync(STATE_FILE, JSON.stringify(state, null, 2));
}

async function main() {
  if (!RPC_URL) fail("ARBITRUM_RPC_URL/ARBITRUM_SEPOLIA_RPC_URL is required");
  if (!ethers.isAddress(VAULT_ADDRESS)) fail("BRIDGE_V3_VAULT_ADDRESS is required");
  if (!RELAYER_PRIVATE_KEY) fail("V3_RELAYER_PRIVATE_KEY is required (gas payer only; not a validator key)");
  if (!Array.isArray(COSIGNERS) || COSIGNERS.length < 2) fail("V3_COSIGNERS_JSON must list independent cosigners");
  if (!Number.isInteger(CONFIRMATIONS) || CONFIRMATIONS < 1) fail("CONFIDENTIAL_CHAIN_CONFIRMATIONS must be >= 1");

  const provider = new ethers.JsonRpcProvider(RPC_URL);
  const relayer = new ethers.Wallet(RELAYER_PRIVATE_KEY, provider);
  const vault = new ethers.Contract(VAULT_ADDRESS, ABI, relayer);
  const threshold = Number(await vault.validatorThreshold());
  if (COSIGNERS.length < threshold) fail(`Need at least ${threshold} configured cosigners`);

  const state = loadState();
  let busy = false;

  async function collect(endpoint, payload) {
    const results = await Promise.allSettled(COSIGNERS.map((c) => requestSignature(c, endpoint, payload)));
    const unique = new Map();
    for (const r of results) {
      if (r.status === "fulfilled" && ethers.isAddress(r.value.signer) && ethers.isHexString(r.value.signature)) {
        unique.set(r.value.signer.toLowerCase(), r.value.signature);
      }
    }
    if (unique.size < threshold) fail(`Cosigner quorum unavailable: ${unique.size}/${threshold}`);
    return Array.from(unique.values()).slice(0, threshold);
  }

  async function finalizeSafeCheckpoint() {
    const status = await getJson(`${NODE_URL}/api/v1/node/status`);
    const currentHeight = BigInt(status.blockchain_height || 0);
    if (currentHeight <= BigInt(CONFIRMATIONS)) return;

    const targetHeight = currentHeight - BigInt(CONFIRMATIONS);
    const latestHeight = BigInt(await vault.latestFinalizedHeight());
    if (targetHeight <= latestHeight) return;

    const parentHash = await vault.latestFinalizedBlockHash();
    const block = await getJson(`${NODE_URL}/api/v1/chain/block/${targetHeight}`);
    const blockHash = block.hash;
    if (!ethers.isHexString(blockHash, 32)) fail("Node returned invalid checkpoint block hash");

    const payload = {
      height: targetHeight.toString(),
      blockHash,
      parentCheckpointHash: parentHash,
    };
    const signatures = await collect("/sign/checkpoint", payload);

    const tx = await vault.finalizeCheckpoint(targetHeight, blockHash, parentHash, signatures);
    console.log(`[V3 RELAYER] Finalizing checkpoint #${targetHeight}: ${tx.hash}`);
    await tx.wait();
    console.log(`[V3 RELAYER] Checkpoint #${targetHeight} finalized.`);
  }

  async function processFinalizedWithdrawals() {
    const checkpointHeight = BigInt(await vault.latestFinalizedHeight());
    const checkpointHash = await vault.latestFinalizedBlockHash();
    if (checkpointHeight === 0n) return;

    const localCheckpoint = await getJson(`${NODE_URL}/api/v1/chain/block/${checkpointHeight}`);
    if ((localCheckpoint.hash || "").toLowerCase() !== checkpointHash.toLowerCase()) {
      fail("Local node no longer contains the on-chain finalized checkpoint; refusing withdrawals");
    }

    let h = BigInt(state.lastWithdrawalHeight || 0) + 1n;
    while (h <= checkpointHeight) {
      const block = await getJson(`${NODE_URL}/api/v1/chain/block/${h}`);
      const withdrawalBlockHash = block.hash;
      if (!ethers.isHexString(withdrawalBlockHash, 32)) fail(`Invalid block hash at height ${h}`);

      for (const w of block.withdrawals || []) {
        const orderId = ethers.keccak256(ethers.toUtf8Bytes(String(w.order_id || "")));
        if (await vault.executedWithdrawals(orderId)) continue;

        const recipient = String(w.destination || "");
        const amount = BigInt(w.net_amount_raw || 0);
        const fee = BigInt(w.fee_raw || 0);
        if (!ethers.isAddress(recipient) || amount <= 0n) fail(`Invalid withdrawal in block ${h}`);

        const payload = {
          orderId,
          recipient,
          amount: amount.toString(),
          fee: fee.toString(),
          withdrawalBlockHeight: h.toString(),
          withdrawalBlockHash,
          checkpointHeight: checkpointHeight.toString(),
          checkpointHash,
        };
        const signatures = await collect("/sign/withdrawal", payload);

        const tx = await vault.withdraw(
          orderId,
          recipient,
          amount,
          fee,
          h,
          withdrawalBlockHash,
          checkpointHeight,
          checkpointHash,
          signatures
        );
        console.log(`[V3 RELAYER] Withdrawal ${w.order_id} submitted: ${tx.hash}`);
        await tx.wait();
        console.log(`[V3 RELAYER] Withdrawal ${w.order_id} confirmed.`);
      }

      state.lastWithdrawalHeight = Number(h);
      saveState(state);
      h += 1n;
    }
  }

  async function tick() {
    if (busy) return;
    busy = true;
    try {
      await finalizeSafeCheckpoint();
      await processFinalizedWithdrawals();
    } catch (err) {
      console.error("[V3 RELAYER]", err.message);
    } finally {
      busy = false;
    }
  }

  console.log(`[V3 RELAYER] Address: ${relayer.address}`);
  console.log(`[V3 RELAYER] Vault: ${VAULT_ADDRESS}`);
  console.log(`[V3 RELAYER] Threshold: ${threshold}/${COSIGNERS.length} configured cosigners`);
  await tick();
  setInterval(tick, POLL_MS);
}

main().catch((err) => {
  console.error("[V3 RELAYER FATAL]", err);
  process.exit(1);
});
