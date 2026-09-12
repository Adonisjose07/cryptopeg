// State
let currentActiveWallet = null;
let lastTumblerPlan = null;

// On Page Load
document.addEventListener("DOMContentLoaded", () => {
  fetchNodeData();
  fetchBlocks();
  updateDepositPreview();
  // Auto-refresh every 4 seconds
  setInterval(fetchNodeData, 4000);
});

// Toast notification helper
function showToast(message, type = "success") {
  const toast = document.getElementById("toast");
  const card = document.getElementById("toast-card");
  const msg = document.getElementById("toast-msg");
  const icon = document.getElementById("toast-icon");

  msg.textContent = message;
  if (type === "success") {
    card.className = "bg-emerald-950/90 border border-emerald-500/50 px-4 py-3 rounded-xl shadow-2xl flex items-center space-x-3 text-xs text-emerald-200";
    icon.innerHTML = `<svg class="w-4 h-4 text-emerald-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 13l4 4L19 7"></path></svg>`;
  } else {
    card.className = "bg-rose-950/90 border border-rose-500/50 px-4 py-3 rounded-xl shadow-2xl flex items-center space-x-3 text-xs text-rose-200";
    icon.innerHTML = `<svg class="w-4 h-4 text-rose-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"></path></svg>`;
  }

  toast.classList.remove("translate-y-20", "opacity-0", "pointer-events-none");
  setTimeout(() => {
    toast.classList.add("translate-y-20", "opacity-0", "pointer-events-none");
  }, 4000);
}

// Tab switcher
function switchTab(tabId) {
  document.querySelectorAll(".tab-content").forEach(el => el.classList.add("hidden"));
  document.querySelectorAll(".tab-btn").forEach(btn => {
    btn.classList.remove("border-emerald-500", "text-emerald-400");
    btn.classList.add("border-transparent", "text-slate-400");
  });

  const activeContent = document.getElementById(tabId);
  const activeBtn = document.getElementById("btn-" + tabId);
  if (activeContent) activeContent.classList.remove("hidden");
  if (activeBtn) {
    activeBtn.classList.remove("border-transparent", "text-slate-400");
    activeBtn.classList.add("border-emerald-500", "text-emerald-400");
  }

  if (tabId === "tab-explorer") {
    fetchBlocks();
  }
}

// Fetch Node Status
async function fetchNodeData() {
  try {
    const res = await fetch("/api/v1/node/status");
    if (!res.ok) throw new Error("HTTP error " + res.status);
    const data = await res.json();

    document.getElementById("metric-collateral").textContent = data.vault.total_collateral_usdt;
    document.getElementById("metric-circulating").textContent = data.vault.circulating_shielded_supply;
    document.getElementById("metric-fee-pool").textContent = data.vault.fee_pool_reserve_usdt;
    const tDisplay = document.getElementById("treasury-pool-display");
    if (tDisplay) tDisplay.textContent = data.vault.fee_pool_reserve_usdt;
    document.getElementById("metric-height").textContent = data.blockchain_height + " bloques";
    document.getElementById("metric-utxos").textContent = data.utxo_pool_count;

    document.getElementById("deposit-fee-rate").textContent = (data.vault.deposit_fee_bps / 100).toFixed(2) + "% (" + data.vault.deposit_fee_bps + " bps)";
    document.getElementById("withdraw-fee-rate").textContent = (data.vault.withdraw_fee_bps / 100).toFixed(2) + "% (" + data.vault.withdraw_fee_bps + " bps)";

    document.getElementById("node-health-text").textContent = "Nodo Conectado (" + data.blockchain_height + " bloques)";
  } catch (err) {
    document.getElementById("node-health-text").textContent = "Conectando al Nodo...";
  }
}

// Fetch Blocks for Explorer
async function fetchBlocks() {
  try {
    const res = await fetch("/api/v1/chain/blocks?limit=25");
    if (!res.ok) return;
    const data = await res.json();
    const tbody = document.getElementById("blocks-tbody");

    if (!data.blocks || data.blocks.length === 0) {
      tbody.innerHTML = `<tr><td colspan="7" class="py-6 text-center text-slate-500">No se encontraron bloques en LMDB.</td></tr>`;
      return;
    }

    tbody.innerHTML = data.blocks.map(b => `
      <tr class="hover:bg-slate-900/80 transition">
        <td class="py-3 px-3 font-bold text-emerald-400">#${b.height}</td>
        <td class="py-3 px-3 text-slate-300 select-all" title="${b.hash}">${b.hash.substring(0, 16)}...</td>
        <td class="py-3 px-3 text-slate-400 select-all" title="${b.merkle_root}">${b.merkle_root.substring(0, 16)}...</td>
        <td class="py-3 px-3 text-slate-400">${new Date(b.timestamp * 1000).toLocaleTimeString()}</td>
        <td class="py-3 px-3 text-center"><span class="px-2 py-0.5 rounded bg-slate-800 text-slate-300 font-semibold">${b.tx_count}</span></td>
        <td class="py-3 px-3 text-center"><span class="px-2 py-0.5 rounded bg-emerald-950 text-emerald-400 font-semibold">${b.deposit_count}</span></td>
        <td class="py-3 px-3 text-center"><span class="px-2 py-0.5 rounded bg-purple-950 text-purple-400 font-semibold">${b.withdrawal_count}</span></td>
      </tr>
    `).join("");
  } catch (err) {
    console.error(err);
  }
}

// Deposit Calculation Preview
function updateDepositPreview() {
  const gross = parseFloat(document.getElementById("deposit-amount").value) || 0;
  const feeRate = 0.005; // 50 bps
  const fee = gross * feeRate;
  const net = gross - fee;

  document.getElementById("prev-dep-gross").textContent = gross.toFixed(6) + " USDT";
  document.getElementById("prev-dep-fee").textContent = "- " + fee.toFixed(6) + " USDT";
  document.getElementById("prev-dep-net").textContent = net.toFixed(6) + " USDT";
}

// Execute Deposit
async function executeDeposit() {
  const btn = document.getElementById("btn-deposit");
  const gross = parseFloat(document.getElementById("deposit-amount").value);
  const recipient = document.getElementById("deposit-recipient").value.trim();

  if (!gross || gross <= 0) {
    showToast("Ingresa un monto válido en USDT", "error");
    return;
  }
  if (!recipient.startsWith("STX")) {
    showToast("Ingresa una dirección Stealth válida (comienza con STX)", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-slate-950" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Minando Bloque en LMDB...`;

  try {
    const res = await fetch("/api/v1/vault/deposit", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ gross_usdt: gross, recipient_stealth_address: recipient })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo en depósito");

    showToast(`¡Acuñación exitosa! Minado en Bloque #${data.block_height}`, "success");
    fetchNodeData();
    fetchBlocks();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = "<span>Acuñar Tokens Privados en LMDB</span>";
  }
}

// Generate Wallet
async function generateWallet() {
  try {
    const res = await fetch("/api/v1/wallet/generate", { method: "POST" });
    const data = await res.json();

    currentActiveWallet = data;
    document.getElementById("wallet-details").classList.remove("hidden");
    document.getElementById("w-addr").textContent = data.stealth_address;
    document.getElementById("w-spend-pub").textContent = data.spend_public_key;
    document.getElementById("w-view-pub").textContent = data.view_public_key;
    document.getElementById("w-spend-priv").textContent = data.spend_private_key;
    document.getElementById("w-view-priv").textContent = data.view_private_key;

    // Auto populate scanner
    document.getElementById("scan-view-priv").value = data.view_private_key;
    document.getElementById("scan-spend-pub").value = data.spend_public_key;

    showToast("Nueva billetera furtiva generada con éxito");
  } catch (err) {
    showToast("Fallo al generar billetera: " + err.message, "error");
  }
}

function useCurrentWalletAsRecipient() {
  if (currentActiveWallet) {
    document.getElementById("deposit-recipient").value = currentActiveWallet.stealth_address;
    showToast("Dirección copiada al campo de depósito");
  } else {
    showToast("Genera primero una billetera en la pestaña 'Billetera'", "error");
    switchTab("tab-wallet");
  }
}

// Scan Wallet Balance by View-Key
async function scanWalletBalance() {
  const viewPriv = document.getElementById("scan-view-priv").value.trim();
  const spendPub = document.getElementById("scan-spend-pub").value.trim();

  if (!viewPriv || !spendPub) {
    showToast("Ingresa View Private Key y Spend Public Key", "error");
    return;
  }

  try {
    const res = await fetch("/api/v1/wallet/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ view_private_key: viewPriv, spend_public_key: spendPub })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo al escanear");

    document.getElementById("scan-results").classList.remove("hidden");
    document.getElementById("scan-total-bal").textContent = data.total_balance_usdt;
    document.getElementById("scan-out-count").textContent = data.outputs_count;

    const list = document.getElementById("scan-utxo-list");
    if (data.outputs.length === 0) {
      list.innerHTML = `<div class="text-xs text-slate-500 py-2">No se detectaron salidas activas para esta identidad.</div>`;
    } else {
      list.innerHTML = data.outputs.map(o => `
        <div class="bg-slate-950 border border-slate-800 p-3 rounded-xl flex flex-col sm:flex-row sm:items-center justify-between gap-2 text-xs">
          <div>
            <div class="font-bold text-emerald-400 mono">${o.amount_usdt}</div>
            <div class="text-[10px] text-slate-400 mono select-all truncate max-w-md">Destino P: ${o.destination_one_time}</div>
          </div>
          <div class="flex space-x-2">
            <button onclick="copyToTxForm('${o.destination_one_time}')" class="bg-purple-950 hover:bg-purple-900 text-purple-300 px-2.5 py-1 rounded text-[11px] font-semibold transition">Usar para Transferir</button>
            <button onclick="copyToWithdrawForm('${o.destination_one_time}')" class="bg-indigo-950 hover:bg-indigo-900 text-indigo-300 px-2.5 py-1 rounded text-[11px] font-semibold transition">Usar para Retirar</button>
          </div>
        </div>
      `).join("");
    }

    showToast("Escaneo completado: " + data.total_balance_usdt + " detectados");
  } catch (err) {
    showToast(err.message, "error");
  }
}

function copyToTxForm(pubkey) {
  document.getElementById("tx-input-pub").value = pubkey;
  if (currentActiveWallet) {
    document.getElementById("tx-spend-priv").value = currentActiveWallet.spend_private_key;
    document.getElementById("tx-view-priv").value = currentActiveWallet.view_private_key;
  }
  switchTab("tab-transfer");
  showToast("UTXO asignado a formulario de transferencia");
}

function copyToWithdrawForm(pubkey) {
  document.getElementById("withdraw-input-pub").value = pubkey;
  if (currentActiveWallet) {
    document.getElementById("withdraw-spend-priv").value = currentActiveWallet.spend_private_key;
    document.getElementById("withdraw-view-priv").value = currentActiveWallet.view_private_key;
  }
  switchTab("tab-vault");
  showToast("UTXO asignado a formulario de retiro");
}

// Execute Shielded Transfer
async function executeTransfer() {
  const btn = document.getElementById("btn-transfer");
  const spendPriv = document.getElementById("tx-spend-priv").value.trim();
  const viewPriv = document.getElementById("tx-view-priv").value.trim();
  const inputPub = document.getElementById("tx-input-pub").value.trim();
  const recipient = document.getElementById("tx-recipient").value.trim();
  const amount = parseFloat(document.getElementById("tx-amount").value);

  if (!spendPriv || !viewPriv || !inputPub || !recipient || !amount) {
    showToast("Completa todos los campos de la transferencia", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Generando Anillo y Minando...`;

  try {
    const res = await fetch("/api/v1/tx/transfer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        sender_spend_private_key: spendPriv,
        sender_view_private_key: viewPriv,
        input_utxo_pubkey: inputPub,
        recipient_stealth_address: recipient,
        amount_usdt: amount
      })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo en transferencia");

    document.getElementById("tx-result").classList.remove("hidden");
    document.getElementById("res-tx-block").textContent = "#" + data.block_height;
    document.getElementById("res-tx-hash").textContent = data.tx_hash;
    document.getElementById("res-tx-keyimg").textContent = data.key_image;
    document.getElementById("res-tx-ring").textContent = data.ring_size;

    showToast(`Transferencia confidencial minada en Bloque #${data.block_height}`, "success");
    fetchNodeData();
    fetchBlocks();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = "<span>Firmar con Anillo (MLSAG) y Enviar</span>";
  }
}

// Execute Withdrawal
async function executeWithdrawal() {
  const btn = document.getElementById("btn-withdraw");
  const spendPriv = document.getElementById("withdraw-spend-priv").value.trim();
  const viewPriv = document.getElementById("withdraw-view-priv").value.trim();
  const inputPub = document.getElementById("withdraw-input-pub").value.trim();
  const amount = parseFloat(document.getElementById("withdraw-amount").value);
  const dest = document.getElementById("withdraw-dest").value.trim();

  if (!spendPriv || !viewPriv || !inputPub || !amount || !dest) {
    showToast("Completa todos los campos para el retiro", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Dispersando en Microtransacciones...`;

  try {
    const res = await fetch("/api/v1/vault/withdraw", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        burner_spend_private_key: spendPriv,
        burner_view_private_key: viewPriv,
        input_utxo_pubkey: inputPub,
        tokens_to_withdraw: amount,
        destination_public_usdt: dest
      })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo en retiro");

    renderTumblerRoutes(data);
    switchTab("tab-tumbler");
    showToast(`Retiro de ${data.net_usdt_tumbled} dispersado en ${data.total_micro_fragments} fragmentos`, "success");
    fetchNodeData();
    fetchBlocks();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = "<span>Ejecutar Retiro y Mezclador</span>";
  }
}

// Render Tumbler Visualizer
function renderTumblerRoutes(plan) {
  lastTumblerPlan = plan;
  document.getElementById("tumbler-empty").classList.add("hidden");
  document.getElementById("tumbler-visual").classList.remove("hidden");

  document.getElementById("tumb-order").textContent = plan.order_id;
  document.getElementById("tumb-burned").textContent = plan.gross_tokens_burned;
  document.getElementById("tumb-fee").textContent = "- " + plan.fee_to_pool;
  document.getElementById("tumb-net").textContent = plan.net_usdt_tumbled;
  document.getElementById("tumb-frag-count").textContent = plan.total_micro_fragments;

  const container = document.getElementById("tumb-routes-container");
  container.innerHTML = plan.routes.map(r => `
    <div class="bg-slate-950 border border-slate-800 p-3.5 rounded-xl space-y-2">
      <div class="flex items-center justify-between text-xs">
        <span class="font-bold text-white">Fragmento #${r.fragment_index}</span>
        <span class="mono text-emerald-400 font-bold">${r.net_amount_usdt}</span>
      </div>
      <div class="text-[11px] text-slate-400 flex items-center justify-between border-t border-slate-900 pt-1.5">
        <span>Ruta de saltos:</span>
        <span class="text-indigo-400 font-semibold">${r.hops_count} saltos efímeros programados</span>
      </div>
    </div>
  `).join("");
}

// Execute Treasury Fee Claim
async function executeClaimFees() {
  const btn = document.getElementById("btn-claim-fees");
  const amountInput = document.getElementById("claim-fee-amount");
  const addrInput = document.getElementById("claim-fee-address");

  const amount = parseFloat(amountInput.value);
  const dest = addrInput.value.trim();

  if (!amount || amount <= 0 || !dest) {
    showToast("Ingresa un monto válido y la dirección de tesorería 0x...", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-slate-950 inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Procesando Retiro de Tesorería...`;

  try {
    const res = await fetch("/api/v1/vault/claim-fees", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        amount_usdt: amount,
        treasury_address: dest
      })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo al reclamar comisiones");

    showToast(`Comisiones cobradas con éxito: ${data.amount_claimed_usdt} hacia ${data.destination_address.substring(0, 10)}...`, "success");
    amountInput.value = "";
    fetchNodeData();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = "<span>Transferir Ganancias a Tesorería</span>";
  }
}

// =================================================================
// WEB3 & METAMASK INTEGRATION (ARBITRUM SEPOLIA)
// =================================================================
const ARB_SEPOLIA_CHAIN_ID = 421614;
const ARB_SEPOLIA_HEX = "0x66eee";
const VAULT_CONTRACT_ADDRESS = "0x0ddFB2b3095DFC50E15bCD37b6A3a786a4DCB3e0";
const USDT_CONTRACT_ADDRESS = "0x900A96C51aac4EB8aF5FDa39bc0Ef13ADBe88B44";

const ERC20_ABI = [
  "function balanceOf(address account) external view returns (uint256)",
  "function allowance(address owner, address spender) external view returns (uint256)",
  "function approve(address spender, uint256 amount) external returns (bool)",
  "function decimals() external view returns (uint8)"
];

const VAULT_ABI = [
  "function deposit(uint256 grossAmount, bytes32 stealthPubView, bytes32 stealthPubSpend) external",
  "function depositFeeBps() external view returns (uint256)",
  "function accumulatedFees() external view returns (uint256)",
  "function getCollateralBalance() external view returns (uint256)",
  "function getCirculatingBacking() external view returns (uint256)"
];

let web3Provider = null;
let web3Signer = null;
let web3UserAddress = null;

// Connect MetaMask
async function connectMetaMask() {
  if (typeof window.ethereum === "undefined") {
    showToast("MetaMask no detectado. Instala MetaMask para interactuar.", "error");
    window.open("https://metamask.io/download/", "_blank");
    return;
  }

  try {
    web3Provider = new ethers.BrowserProvider(window.ethereum);
    const accounts = await web3Provider.send("eth_requestAccounts", []);
    if (!accounts || accounts.length === 0) {
      showToast("No se seleccionó ninguna cuenta en MetaMask", "error");
      return;
    }

    web3Signer = await web3Provider.getSigner();
    web3UserAddress = await web3Signer.getAddress();

    // Verify & Switch Network to Arbitrum Sepolia
    const network = await web3Provider.getNetwork();
    if (Number(network.chainId) !== ARB_SEPOLIA_CHAIN_ID) {
      await switchOrAddArbitrumSepolia();
    }

    await updateWeb3UI();
    showToast("MetaMask conectado a Arbitrum Sepolia", "success");
  } catch (err) {
    console.error("Error conectando MetaMask:", err);
    showToast(err.message || "Error al conectar MetaMask", "error");
  }
}

// Switch or Add Arbitrum Sepolia network to MetaMask
async function switchOrAddArbitrumSepolia() {
  try {
    await window.ethereum.request({
      method: "wallet_switchEthereumChain",
      params: [{ chainId: ARB_SEPOLIA_HEX }]
    });
  } catch (switchError) {
    if (switchError.code === 4902) {
      await window.ethereum.request({
        method: "wallet_addEthereumChain",
        params: [{
          chainId: ARB_SEPOLIA_HEX,
          chainName: "Arbitrum Sepolia",
          nativeCurrency: { name: "Ether", symbol: "ETH", decimals: 18 },
          rpcUrls: ["https://sepolia-rollup.arbitrum.io/rpc"],
          blockExplorerUrls: ["https://sepolia.arbiscan.io"]
        }]
      });
    } else {
      throw switchError;
    }
  }
}

// Update UI elements with MetaMask account and balances
async function updateWeb3UI() {
  if (!web3UserAddress || !web3Provider) return;

  const shortAddr = `${web3UserAddress.slice(0, 6)}...${web3UserAddress.slice(-4)}`;
  const statusBtn = document.getElementById("btn-connect-metamask");
  const statusText = document.getElementById("metamask-status-text");
  if (statusBtn && statusText) {
    statusText.textContent = shortAddr;
    statusBtn.classList.remove("border-amber-500/40", "text-amber-300");
    statusBtn.classList.add("border-emerald-500/50", "text-emerald-300", "bg-emerald-950/40");
  }

  try {
    const ethBal = await web3Provider.getBalance(web3UserAddress);
    const formattedEth = parseFloat(ethers.formatEther(ethBal)).toFixed(4);

    const usdtContract = new ethers.Contract(USDT_CONTRACT_ADDRESS, ERC20_ABI, web3Provider);
    const usdtBal = await usdtContract.balanceOf(web3UserAddress);
    const formattedUsdt = parseFloat(ethers.formatUnits(usdtBal, 6)).toFixed(2);

    const web3InfoBox = document.getElementById("web3-wallet-info");
    if (web3InfoBox) {
      web3InfoBox.classList.remove("hidden");
      document.getElementById("web3-user-address").textContent = shortAddr;
      document.getElementById("web3-usdt-balance").textContent = `${formattedUsdt} USDT`;
      document.getElementById("web3-eth-balance").textContent = `${formattedEth} ETH`;
    }
  } catch (e) {
    console.error("Error leyendo balances Web3:", e);
  }
}

// Helper: Generate a fresh stealth address and auto-paste
async function generateAndUseNewStealthAddress() {
  await generateWallet();
  useCurrentWalletAsRecipient();
}

// Helper: Calculate dynamic gas overrides with safety buffer for Arbitrum Sepolia
async function getArbitrumTxOverrides(provider) {
  try {
    const feeData = await provider.getFeeData();
    const block = await provider.getBlock("latest");
    const currentBaseFee = block && block.baseFeePerGas ? block.baseFeePerGas : (feeData.gasPrice || 350000000n);
    
    // Buffer del 100% sobre el baseFee (2x) para absorber fluctuaciones rápidas de bloques en Arbitrum
    const maxFee = (currentBaseFee * 200n) / 100n;
    const priorityFee = feeData.maxPriorityFeePerGas && feeData.maxPriorityFeePerGas > 0n 
      ? (feeData.maxPriorityFeePerGas * 150n) / 100n 
      : 20000000n; // 0.02 Gwei

    return {
      maxFeePerGas: maxFee + priorityFee,
      maxPriorityFeePerGas: priorityFee
    };
  } catch (err) {
    console.warn("No se pudieron calcular overrides dinámicos de gas:", err);
    return {};
  }
}

// Execute MetaMask Deposit to CryptoPegVault
async function executeMetaMaskDeposit() {
  if (!web3Signer) {
    await connectMetaMask();
    if (!web3Signer) return;
  }

  const grossVal = parseFloat(document.getElementById("deposit-amount").value);
  let recipient = document.getElementById("deposit-recipient").value.trim();

  if (!grossVal || grossVal <= 0) {
    showToast("Ingresa un monto válido en USDT a depositar", "error");
    return;
  }

  // If no stealth recipient provided, generate one automatically
  if (!recipient) {
    await generateAndUseNewStealthAddress();
    recipient = document.getElementById("deposit-recipient").value.trim();
  }

  if (!recipient.startsWith("STX") || recipient.length !== 131) {
    showToast("La dirección furtiva debe ser una clave DKSAP válida (STX... de 131 caracteres)", "error");
    return;
  }

  const stealthSpend = "0x" + recipient.substring(3, 67);
  const stealthView = "0x" + recipient.substring(67, 131);
  const depositAmountUnits = ethers.parseUnits(grossVal.toString(), 6);

  const btn = document.getElementById("btn-deposit-metamask");
  const origHtml = btn.innerHTML;
  btn.disabled = true;

  try {
    const usdtContract = new ethers.Contract(USDT_CONTRACT_ADDRESS, ERC20_ABI, web3Signer);

    // Step 1: Check allowance
    btn.innerHTML = `<span class="animate-spin inline-block mr-2">🔄</span> 1/2: Verificando / Aprobando USDT...`;
    const currentAllowance = await usdtContract.allowance(web3UserAddress, VAULT_CONTRACT_ADDRESS);

    if (currentAllowance < depositAmountUnits) {
      showToast("Confirma la aprobación de USDT en MetaMask...", "success");
      const approveOverrides = await getArbitrumTxOverrides(web3Provider);
      const approveTx = await usdtContract.approve(VAULT_CONTRACT_ADDRESS, depositAmountUnits, {
        ...approveOverrides,
        gasLimit: 120000n
      });
      btn.innerHTML = `<span class="animate-spin inline-block mr-2">⏳</span> Esperando confirmación de aprobación en Arbitrum...`;
      await approveTx.wait();
      showToast("¡USDT Aprobado con éxito! Ahora confirma el depósito...", "success");
    }

    // Step 2: Execute deposit
    btn.innerHTML = `<span class="animate-spin inline-block mr-2">🚀</span> 2/2: Confirmando depósito en CryptoPegVault...`;
    const vaultContract = new ethers.Contract(VAULT_CONTRACT_ADDRESS, VAULT_ABI, web3Signer);
    
    const depOverrides = await getArbitrumTxOverrides(web3Provider);
    let gasLimit = 350000n;
    try {
      const est = await vaultContract.deposit.estimateGas(depositAmountUnits, stealthView, stealthSpend);
      gasLimit = (est * 135n) / 100n;
    } catch (e) {
      console.warn("Usando gasLimit seguro por defecto:", e);
    }

    const depositTx = await vaultContract.deposit(depositAmountUnits, stealthView, stealthSpend, {
      ...depOverrides,
      gasLimit
    });

    btn.innerHTML = `<span class="animate-spin inline-block mr-2">⛓️</span> Minando bloque en Arbitrum Sepolia...`;
    showToast(`Tx enviada: ${depositTx.hash.slice(0, 14)}...`, "success");

    const receipt = await depositTx.wait();
    btn.disabled = false;
    btn.innerHTML = origHtml;

    showToast("¡Depósito en Arbitrum confirmado! El oráculo de Docker lo acuñará en segundos.", "success");

    // Display banner with Arbiscan link
    const banner = document.getElementById("last-tx-banner");
    if (banner) {
      banner.classList.remove("hidden");
      const link = document.getElementById("last-tx-link");
      link.href = `https://sepolia.arbiscan.io/tx/${depositTx.hash}`;
      link.textContent = `Tx: ${depositTx.hash.slice(0, 14)}... (Ver en Arbiscan ↗)`;
      document.getElementById("last-tx-block").textContent = receipt.blockNumber;
    }

    await updateWeb3UI();
    // Wait for the oracle to process and refresh data
    setTimeout(fetchNodeData, 4000);
    setTimeout(fetchNodeData, 8000);
  } catch (err) {
    console.error("Error en depósito MetaMask:", err);
    btn.disabled = false;
    btn.innerHTML = origHtml;
    let friendlyMsg = err.reason || err.message || "Transacción cancelada o fallida";
    if (friendlyMsg.includes("user rejected") || friendlyMsg.includes("ACTION_REJECTED")) {
      friendlyMsg = "Transacción cancelada por el usuario en MetaMask.";
    } else if (friendlyMsg.includes("max fee per gas less than block base fee")) {
      friendlyMsg = "La tarifa base de Arbitrum osciló rápidamente. Hemos ajustado el buffer automático, por favor reintenta el depósito.";
    }
    showToast(friendlyMsg, "error");
  }
}

// Setup MetaMask listeners on page load
if (typeof window !== "undefined" && window.ethereum) {
  window.ethereum.on("accountsChanged", (accounts) => {
    if (accounts.length > 0) {
      web3UserAddress = accounts[0];
      updateWeb3UI();
    } else {
      web3UserAddress = null;
      location.reload();
    }
  });

  window.ethereum.on("chainChanged", () => location.reload());
}

