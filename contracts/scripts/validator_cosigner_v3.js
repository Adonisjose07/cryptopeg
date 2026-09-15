const http = require("http");
const { ethers } = require("ethers");
require("dotenv").config();

const RPC_URL = process.env.ARBITRUM_SEPOLIA_RPC_URL || process.env.ARBITRUM_RPC_URL;
const VAULT_ADDRESS = process.env.BRIDGE_V3_VAULT_ADDRESS || "";
const NODE_URL = (process.env.NODE_DAEMON_URL || "http://127.0.0.1:8080").replace(/\/$/, "");
const PRIVATE_KEY = process.env.BRIDGE_VALIDATOR_PRIVATE_KEY || "";
const AUTH_TOKEN = process.env.COSIGNER_AUTH_TOKEN || "";
const PORT = Number(process.env.COSIGNER_PORT || "8787");
const CONFIRMATIONS = Number(process.env.CONFIDENTIAL_CHAIN_CONFIRMATIONS || "6");

const ABI = [
  "function latestFinalizedHeight() view returns (uint64)",
  "function latestFinalizedBlockHash() view returns (bytes32)",
  "function isValidator(address) view returns (bool)"
];

function fail(message) {
  throw new Error(message);
}

async function getJson(url) {
  const res = await fetch(url);
  if (!res.ok) fail(`HTTP ${res.status} from ${url}`);
  return res.json();
}

async function main() {
  if (!RPC_URL) fail("ARBITRUM_RPC_URL/ARBITRUM_SEPOLIA_RPC_URL is required");
  if (!ethers.isAddress(VAULT_ADDRESS)) fail("BRIDGE_V3_VAULT_ADDRESS is required");
  if (!PRIVATE_KEY) fail("BRIDGE_VALIDATOR_PRIVATE_KEY is required");
  if (!AUTH_TOKEN) fail("COSIGNER_AUTH_TOKEN is required (fail-closed)");
  if (!Number.isInteger(CONFIRMATIONS) || CONFIRMATIONS < 1) fail("CONFIDENTIAL_CHAIN_CONFIRMATIONS must be >= 1");

  const provider = new ethers.JsonRpcProvider(RPC_URL);
  const wallet = new ethers.Wallet(PRIVATE_KEY);
  const vault = new ethers.Contract(VAULT_ADDRESS, ABI, provider);
  if (!(await vault.isValidator(wallet.address))) {
    fail(`Cosigner ${wallet.address} is not an authorized V3 validator`);
  }

  const network = await provider.getNetwork();
  const domain = {
    name: "CryptoPegVault",
    version: "3",
    chainId: network.chainId,
    verifyingContract: VAULT_ADDRESS,
  };

  const checkpointTypes = {
    Checkpoint: [
      { name: "height", type: "uint64" },
      { name: "blockHash", type: "bytes32" },
      { name: "parentCheckpointHash", type: "bytes32" },
    ],
  };

  const withdrawalTypes = {
    Withdrawal: [
      { name: "orderId", type: "bytes32" },
      { name: "recipient", type: "address" },
      { name: "amount", type: "uint256" },
      { name: "fee", type: "uint256" },
      { name: "withdrawalBlockHeight", type: "uint64" },
      { name: "withdrawalBlockHash", type: "bytes32" },
      { name: "checkpointHeight", type: "uint64" },
      { name: "checkpointHash", type: "bytes32" },
    ],
  };

  async function verifyCanonicalCheckpoint(height, blockHash, parentCheckpointHash) {
    const latestHeight = BigInt(await vault.latestFinalizedHeight());
    const latestHash = (await vault.latestFinalizedBlockHash()).toLowerCase();
    if (BigInt(height) <= latestHeight) fail("Checkpoint height does not advance on-chain finality");
    if (parentCheckpointHash.toLowerCase() !== latestHash) fail("Checkpoint parent does not match on-chain finality");

    const status = await getJson(`${NODE_URL}/api/v1/node/status`);
    const currentHeight = BigInt(status.blockchain_height || 0);
    if (currentHeight < BigInt(height) + BigInt(CONFIRMATIONS)) {
      fail("Checkpoint has insufficient confidential-chain confirmations");
    }

    const candidate = await getJson(`${NODE_URL}/api/v1/chain/block/${height}`);
    if ((candidate.hash || "").toLowerCase() !== blockHash.toLowerCase()) {
      fail("Local node disagrees with proposed checkpoint hash");
    }

    if (latestHeight > 0n) {
      const localParent = await getJson(`${NODE_URL}/api/v1/chain/block/${latestHeight}`);
      if ((localParent.hash || "").toLowerCase() !== latestHash) {
        fail("Local node does not contain the currently finalized checkpoint; possible deep fork");
      }
    }
  }

  async function signCheckpoint(body) {
    const height = BigInt(body.height);
    const blockHash = String(body.blockHash || "");
    const parentCheckpointHash = String(body.parentCheckpointHash || "");
    if (!ethers.isHexString(blockHash, 32) || !ethers.isHexString(parentCheckpointHash, 32)) {
      fail("Invalid checkpoint hash fields");
    }
    await verifyCanonicalCheckpoint(height, blockHash, parentCheckpointHash);
    const signature = await wallet.signTypedData(domain, checkpointTypes, {
      height,
      blockHash,
      parentCheckpointHash,
    });
    return { signer: wallet.address, signature };
  }

  async function signWithdrawal(body) {
    const value = {
      orderId: String(body.orderId || ""),
      recipient: String(body.recipient || ""),
      amount: BigInt(body.amount),
      fee: BigInt(body.fee),
      withdrawalBlockHeight: BigInt(body.withdrawalBlockHeight),
      withdrawalBlockHash: String(body.withdrawalBlockHash || ""),
      checkpointHeight: BigInt(body.checkpointHeight),
      checkpointHash: String(body.checkpointHash || ""),
    };

    if (!ethers.isHexString(value.orderId, 32)) fail("Invalid orderId");
    if (!ethers.isAddress(value.recipient)) fail("Invalid recipient");
    if (!ethers.isHexString(value.withdrawalBlockHash, 32)) fail("Invalid withdrawalBlockHash");
    if (!ethers.isHexString(value.checkpointHash, 32)) fail("Invalid checkpointHash");
    if (value.amount <= 0n) fail("Invalid withdrawal amount");

    const latestHeight = BigInt(await vault.latestFinalizedHeight());
    const latestHash = (await vault.latestFinalizedBlockHash()).toLowerCase();
    if (value.checkpointHeight !== latestHeight || value.checkpointHash.toLowerCase() !== latestHash) {
      fail("Requested withdrawal is not bound to the current on-chain checkpoint");
    }
    if (value.withdrawalBlockHeight > latestHeight) fail("Withdrawal block is not finalized");

    const localCheckpoint = await getJson(`${NODE_URL}/api/v1/chain/block/${latestHeight}`);
    if ((localCheckpoint.hash || "").toLowerCase() !== latestHash) {
      fail("Local node is on a fork that does not contain the finalized checkpoint");
    }

    const block = await getJson(`${NODE_URL}/api/v1/chain/block/${value.withdrawalBlockHeight}`);
    if ((block.hash || "").toLowerCase() !== value.withdrawalBlockHash.toLowerCase()) {
      fail("Withdrawal block hash does not match local canonical chain");
    }

    const match = (block.withdrawals || []).find((w) => {
      const hashedOrder = ethers.keccak256(ethers.toUtf8Bytes(String(w.order_id || "")));
      return hashedOrder.toLowerCase() === value.orderId.toLowerCase()
        && String(w.destination || "").toLowerCase() === value.recipient.toLowerCase()
        && BigInt(w.net_amount_raw || 0) === value.amount
        && BigInt(w.fee_raw || 0) === value.fee;
    });
    if (!match) fail("Withdrawal payload is not present in the local finalized block");

    const signature = await wallet.signTypedData(domain, withdrawalTypes, value);
    return { signer: wallet.address, signature };
  }

  const server = http.createServer((req, res) => {
    const send = (status, obj) => {
      res.writeHead(status, { "content-type": "application/json" });
      res.end(JSON.stringify(obj));
    };

    if (req.url === "/health" && req.method === "GET") {
      return send(200, { status: "ok", signer: wallet.address });
    }

    const auth = req.headers.authorization || "";
    if (auth !== `Bearer ${AUTH_TOKEN}`) return send(401, { error: "unauthorized" });
    if (req.method !== "POST") return send(405, { error: "method not allowed" });

    let raw = "";
    req.on("data", (chunk) => {
      raw += chunk;
      if (raw.length > 64 * 1024) req.destroy();
    });
    req.on("end", async () => {
      try {
        const body = JSON.parse(raw || "{}");
        if (req.url === "/sign/checkpoint") return send(200, await signCheckpoint(body));
        if (req.url === "/sign/withdrawal") return send(200, await signWithdrawal(body));
        return send(404, { error: "not found" });
      } catch (err) {
        return send(400, { error: err.message });
      }
    });
  });

  server.listen(PORT, "0.0.0.0", () => {
    console.log(`[V3 COSIGNER] ${wallet.address} listening on :${PORT}`);
    console.log(`[V3 COSIGNER] Node: ${NODE_URL}`);
    console.log(`[V3 COSIGNER] Vault: ${VAULT_ADDRESS}`);
  });
}

main().catch((err) => {
  console.error("[V3 COSIGNER FATAL]", err);
  process.exit(1);
});
