// =================================================================
// CryptoPeg USDT Dashboard - Open Design System (El Origen Standard)
// =================================================================

// State
let currentActiveWallet = null;
let lastTumblerPlan = null;
let wasmCryptoModule = null;
let isWasmLoaded = false;

// Initialize WebAssembly Cryptographic Core (Phase 3 Non-Custodial)
async function initWasmCrypto() {
  try {
    if (typeof CryptoPegWasmModule === "function") {
      wasmCryptoModule = await CryptoPegWasmModule();
      if (wasmCryptoModule && wasmCryptoModule.initSodium && wasmCryptoModule.initSodium()) {
        isWasmLoaded = true;
        console.log("[WASM-CRYPTO] Módulo Criptográfico Wasm inicializado (Modo 100% No-Custodial)");
        updateWasmBadge(true);
        return;
      }
    }
  } catch (e) {
    console.info("[WASM-CRYPTO] Wasm module initialization skipped:", e.message);
  }
  updateWasmBadge(false);
}

function updateWasmBadge(active) {
  const badge = document.getElementById("wasm-badge");
  if (badge) {
    if (active && isWasmLoaded) {
      badge.className = "text-[10px] uppercase font-bold tracking-wider bg-[#00A76F]/15 text-[#00A76F] border border-[#00A76F]/30 px-2.5 py-0.5 rounded-full inline-flex items-center";
      badge.innerHTML = `<span class="w-1.5 h-1.5 rounded-full bg-[#00A76F] mr-1"></span>Wasm SDK Listo`;
    } else {
      badge.className = "text-[10px] uppercase font-bold tracking-wider bg-[#00B8D9]/15 text-[#00B8D9] border border-[#00B8D9]/30 px-2.5 py-0.5 rounded-full inline-flex items-center";
      badge.innerHTML = `<span class="w-1.5 h-1.5 rounded-full bg-[#00B8D9] mr-1"></span>Daemon Local 127.0.0.1`;
    }
  }
}

// On Page Load
document.addEventListener("DOMContentLoaded", () => {
  initWasmCrypto();
  fetchNodeData();
  fetchBlocks();
  updateDepositPreview();
  setupInputListeners();
  // Auto-refresh every 4 seconds
  setInterval(fetchNodeData, 4000);
});

// Toast notification helper with Open Design System tokens
function showToast(message, type = "success") {
  const toast = document.getElementById("toast");
  const card = document.getElementById("toast-card");
  const msg = document.getElementById("toast-msg");
  const icon = document.getElementById("toast-icon");

  msg.textContent = message;

  if (type === "success") {
    card.className = "bg-[#212B36] border border-[#00A76F]/60 px-4 py-3.5 rounded-xl shadow-elevated flex items-center space-x-3 text-xs max-w-md text-[#F9FAFB]";
    icon.innerHTML = `<svg class="w-4 h-4 text-[#00A76F] shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M5 13l4 4L19 7"></path></svg>`;
  } else if (type === "error") {
    card.className = "bg-[#212B36] border border-[#FF5630]/60 px-4 py-3.5 rounded-xl shadow-elevated flex items-center space-x-3 text-xs max-w-md text-[#F9FAFB]";
    icon.innerHTML = `<svg class="w-4 h-4 text-[#FF5630] shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M6 18L18 6M6 6l12 12"></path></svg>`;
  } else if (type === "warning") {
    card.className = "bg-[#212B36] border border-[#FFAB00]/60 px-4 py-3.5 rounded-xl shadow-elevated flex items-center space-x-3 text-xs max-w-md text-[#F9FAFB]";
    icon.innerHTML = `<svg class="w-4 h-4 text-[#FFAB00] shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"></path></svg>`;
  } else {
    // info
    card.className = "bg-[#212B36] border border-[#00B8D9]/60 px-4 py-3.5 rounded-xl shadow-elevated flex items-center space-x-3 text-xs max-w-md text-[#F9FAFB]";
    icon.innerHTML = `<svg class="w-4 h-4 text-[#00B8D9] shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>`;
  }

  toast.classList.remove("translate-y-20", "opacity-0", "pointer-events-none");
  setTimeout(() => {
    toast.classList.add("translate-y-20", "opacity-0", "pointer-events-none");
  }, 4500);
}

// Copy to Clipboard helper
function copyToClipboard(text, label = "Dato") {
  if (!text) return;
  navigator.clipboard.writeText(text).then(() => {
    showToast(`Copiado al portapapeles: ${label}`, "info");
  }).catch(() => {
    showToast(`Error al copiar ${label}`, "error");
  });
}

// Quick amount selector for deposit
function setDepositAmount(val) {
  const input = document.getElementById("deposit-amount");
  if (input) {
    input.value = val;
    clearInputError(input);
    updateDepositPreview();
  }
}

// Visual error helpers for inputs
function setInputError(el, hasError) {
  if (!el) return;
  if (hasError) {
    el.classList.add("border-[#FF5630]", "ring-1", "ring-[#FF5630]");
    el.classList.remove("border-[rgba(145,158,171,0.16)]");
  } else {
    el.classList.remove("border-[#FF5630]", "ring-1", "ring-[#FF5630]");
    el.classList.add("border-[rgba(145,158,171,0.16)]");
  }
}

function clearInputError(el) {
  setInputError(el, false);
}

function setupInputListeners() {
  const inputs = document.querySelectorAll("input");
  inputs.forEach(inp => {
    inp.addEventListener("input", () => clearInputError(inp));
  });
}

// Tab switcher with Open Design System styling
function switchTab(tabId) {
  document.querySelectorAll(".tab-content").forEach(el => el.classList.add("hidden"));
  document.querySelectorAll(".tab-btn").forEach(btn => {
    btn.classList.remove("border-[#00A76F]", "text-[#F9FAFB]", "bg-[#212B36]/60");
    btn.classList.add("border-transparent", "text-[#919EAB]");
  });

  const activeContent = document.getElementById(tabId);
  const activeBtn = document.getElementById("btn-" + tabId);
  if (activeContent) activeContent.classList.remove("hidden");
  if (activeBtn) {
    activeBtn.classList.remove("border-transparent", "text-[#919EAB]");
    activeBtn.classList.add("border-[#00A76F]", "text-[#F9FAFB]", "bg-[#212B36]/60");
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

    const healthDot = document.getElementById("node-health-dot");
    const healthText = document.getElementById("node-health-text");
    if (healthDot && healthText) {
      healthDot.className = "w-2.5 h-2.5 rounded-full bg-[#00A76F] pulse-dot";
      healthText.textContent = "Nodo Conectado (" + data.blockchain_height + " blq)";
      healthText.className = "text-[#F9FAFB] font-medium";
    }
  } catch (err) {
    const healthDot = document.getElementById("node-health-dot");
    const healthText = document.getElementById("node-health-text");
    if (healthDot && healthText) {
      healthDot.className = "w-2.5 h-2.5 rounded-full bg-[#FFAB00] animate-ping";
      healthText.textContent = "Reconectando al Nodo...";
      healthText.className = "text-[#FFAB00] font-medium";
    }
  }
}

// Fetch Blocks for Explorer
async function fetchBlocks() {
  const tbody = document.getElementById("blocks-tbody");
  if (!tbody) return;

  try {
    const res = await fetch("/api/v1/chain/blocks?limit=25");
    if (!res.ok) throw new Error("Error HTTP " + res.status);
    const data = await res.json();

    // Estado Vacío (Obligatorio UI)
    if (!data.blocks || data.blocks.length === 0) {
      tbody.innerHTML = `
        <tr>
          <td colspan="7" class="py-12 text-center text-[#919EAB]">
            <div class="max-w-xs mx-auto space-y-2">
              <svg class="w-10 h-10 mx-auto text-[#637381]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M20 13V6a2 2 0 00-2-2H6a2 2 0 00-2 2v7m16 0v5a2 2 0 01-2 2H6a2 2 0 01-2-2v-5m16 0h-2.586a1 1 0 00-.707.293l-2.414 2.414a1 1 0 01-.707.293h-3.172a1 1 0 01-.707-.293l-2.414-2.414A1 1 0 006.586 13H4"></path>
              </svg>
              <div class="text-sm font-semibold text-[#F9FAFB]">Sin Bloques en el Ledger</div>
              <div class="text-xs text-[#919EAB]">No se encontraron bloques registrados en LMDB.</div>
            </div>
          </td>
        </tr>`;
      return;
    }

    tbody.innerHTML = data.blocks.map(b => `
      <tr class="hover:bg-[#161C24]/80 transition">
        <td class="py-3 px-4 font-bold text-[#00A76F]">#${b.height}</td>
        <td class="py-3 px-4 text-[#919EAB] hover:text-[#F9FAFB] cursor-pointer transition select-all" onclick="copyToClipboard('${b.hash}', 'Hash del Bloque #${b.height}')" title="Clic para copiar: ${b.hash}">
          ${b.hash.substring(0, 16)}...
        </td>
        <td class="py-3 px-4 text-[#919EAB] hover:text-[#F9FAFB] cursor-pointer transition select-all" onclick="copyToClipboard('${b.merkle_root}', 'Raíz Merkle #${b.height}')" title="Clic para copiar: ${b.merkle_root}">
          ${b.merkle_root.substring(0, 16)}...
        </td>
        <td class="py-3 px-4 text-[#919EAB] text-[11px]">${new Date(b.timestamp * 1000).toLocaleTimeString()}</td>
        <td class="py-3 px-4 text-center"><span class="px-2 py-0.5 rounded-md bg-[#161C24] text-[#00B8D9] border border-[#00B8D9]/25 font-semibold text-[11px]">${b.tx_count}</span></td>
        <td class="py-3 px-4 text-center"><span class="px-2 py-0.5 rounded-md bg-[#00A76F]/10 text-[#00A76F] border border-[#00A76F]/30 font-semibold text-[11px]">${b.deposit_count}</span></td>
        <td class="py-3 px-4 text-center"><span class="px-2 py-0.5 rounded-md bg-[#8E33FF]/10 text-[#8E33FF] border border-[#8E33FF]/30 font-semibold text-[11px]">${b.withdrawal_count}</span></td>
      </tr>
    `).join("");
  } catch (err) {
    console.error("Error al cargar bloques:", err);
    tbody.innerHTML = `
      <tr>
        <td colspan="7" class="py-6 text-center text-[#FF5630] text-xs">
          Error al conectar con la base de datos de bloques LMDB.
        </td>
      </tr>`;
  }
}

// Deposit Calculation Preview
function updateDepositPreview() {
  const amountInput = document.getElementById("deposit-amount");
  const gross = parseFloat(amountInput.value) || 0;
  const feeRate = 0.005; // 50 bps
  const fee = gross * feeRate;
  const net = gross - fee;

  document.getElementById("prev-dep-gross").textContent = gross.toFixed(6) + " USDT";
  document.getElementById("prev-dep-fee").textContent = "- " + fee.toFixed(6) + " USDT";
  document.getElementById("prev-dep-net").textContent = net.toFixed(6) + " USDT";
}

// Local cache for spent outputs to maintain 100% privacy without exposing spend_private_key to remote nodes
function markUtxoSpentLocally(pubkey) {
  if (!pubkey) return;
  try {
    const spent = JSON.parse(localStorage.getItem("cryptopeg_spent_utxos") || "[]");
    if (!spent.includes(pubkey)) {
      spent.push(pubkey);
      localStorage.setItem("cryptopeg_spent_utxos", JSON.stringify(spent));
    }
  } catch (e) {}
}

function unmarkUtxoSpentLocally(pubkey) {
  if (!pubkey) return;
  try {
    let spent = JSON.parse(localStorage.getItem("cryptopeg_spent_utxos") || "[]");
    spent = spent.filter(p => p !== pubkey);
    localStorage.setItem("cryptopeg_spent_utxos", JSON.stringify(spent));
  } catch (e) {}
}

function isUtxoSpentLocally(pubkey) {
  if (!pubkey) return false;
  try {
    const spent = JSON.parse(localStorage.getItem("cryptopeg_spent_utxos") || "[]");
    return spent.includes(pubkey);
  } catch (e) {
    return false;
  }
}

function manuallyMarkSpent(pubkey) {
  markUtxoSpentLocally(pubkey);
  showToast("Salida archivada como gastada", "info");
  scanWalletBalance();
}

function manuallyUnmarkSpent(pubkey) {
  unmarkUtxoSpentLocally(pubkey);
  showToast("Salida restaurada a disponibles", "success");
  scanWalletBalance();
}

// Generate Stealth Wallet
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
    const spInput = document.getElementById("scan-spend-priv");
    if (spInput) spInput.value = data.spend_private_key;

    showToast("Nueva billetera furtiva generada con éxito", "success");
  } catch (err) {
    showToast("Fallo al generar billetera: " + err.message, "error");
  }
}

function useCurrentWalletAsRecipient() {
  if (currentActiveWallet) {
    const recip = document.getElementById("deposit-recipient");
    recip.value = currentActiveWallet.stealth_address;
    clearInputError(recip);
    showToast("Dirección furtiva pegada al formulario de depósito", "success");
  } else {
    showToast("Genera primero una billetera en la pestaña 'Billetera'", "warning");
    switchTab("tab-wallet");
  }
}

// Scan Wallet Balance (Modo Sólo Vista o Modo Verificación Completa con Spend Key)
async function scanWalletBalance() {
  const btn = document.getElementById("btn-scan");
  const viewPrivInput = document.getElementById("scan-view-priv");
  const spendPubInput = document.getElementById("scan-spend-pub");
  const spendPrivInput = document.getElementById("scan-spend-priv");

  const viewPriv = viewPrivInput.value.trim();
  const spendPub = spendPubInput.value.trim();
  const spendPriv = spendPrivInput ? spendPrivInput.value.trim() : "";

  let hasError = false;
  if (!viewPriv) {
    setInputError(viewPrivInput, true);
    hasError = true;
  }
  if (!spendPub) {
    setInputError(spendPubInput, true);
    hasError = true;
  }
  if (hasError) {
    showToast("Ingresa View Private Key y Spend Public Key", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-[#161C24] inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Escaneando Ledger LMDB...`;

  try {
    const payload = {
      view_private_key: viewPriv,
      spend_public_key: spendPub
    };
    // Por seguridad no-custodial (V4-04), NUNCA transmitir spend_private_key por HTTP.
    // El filtrado de salidas gastadas se realiza localmente en el navegador.

    const res = await fetch("/api/v1/wallet/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Fallo al escanear");

    document.getElementById("scan-results").classList.remove("hidden");
    const modeBanner = document.getElementById("scan-mode-banner");
    const balLabel = document.getElementById("scan-bal-label");
    const balBadge = document.getElementById("scan-bal-badge");
    const totalBal = document.getElementById("scan-total-bal");
    const countLabel = document.getElementById("scan-count-label");
    const activeTitle = document.getElementById("scan-active-title");
    const utxoCount = document.getElementById("scan-out-count");
    const list = document.getElementById("scan-utxo-list");
    const spentSection = document.getElementById("scan-spent-section");
    const spentList = document.getElementById("scan-spent-list");
    const spentCount = document.getElementById("scan-spent-count");

    totalBal.textContent = data.total_balance_usdt;
    utxoCount.textContent = data.outputs_count;

    if (data.is_watch_only) {
      // 1. MODO SÓLO LECTURA (Watch-Only - Sin Spend Key)
      balLabel.textContent = "Volumen Histórico Recibido (Bruto):";
      balBadge.textContent = "MODO SÓLO LECTURA (WATCH-ONLY)";
      balBadge.className = "text-[10px] px-2 py-0.5 rounded font-semibold uppercase bg-[#00B8D9]/15 text-[#00B8D9] border border-[#00B8D9]/30";
      totalBal.className = "text-2xl font-bold mono mt-1 text-[#00B8D9]";
      countLabel.textContent = "Salidas Recibidas Totales:";
      activeTitle.textContent = "Salidas Detectadas en Cadena:";

      modeBanner.className = "bg-[#00B8D9]/10 border border-[#00B8D9]/25 rounded-xl p-4 text-xs text-[#919EAB] flex items-start space-x-3";
      modeBanner.innerHTML = `
        <span class="p-1.5 bg-[#00B8D9]/15 text-[#00B8D9] rounded-lg shrink-0 mt-0.5">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>
        </span>
        <div class="leading-relaxed space-y-1">
          <div class="text-[#00B8D9] font-bold text-xs">Aviso de Modo Consulta (Solo View-Key)</div>
          <div>Este monto de <strong class="text-[#F9FAFB] font-mono">${data.total_balance_usdt}</strong> representa el <strong>volumen total histórico recibido</strong> en tu dirección furtiva. Como las firmas de anillo RingCT MLSAG ocultan qué salidas fueron consumidas, una View-Key por sí sola no puede deducir los retiros o transferencias salientes.</div>
          <div class="text-[#F9FAFB] font-medium pt-1">👉 Para verificar qué salidas ya gastaste y calcular tu <strong>Saldo Neto Real Disponible</strong>, ingresa tu <code class="text-[#FFAB00] font-mono">Spend Private Key</code> en el formulario de arriba.</div>
        </div>
      `;

      if (spentSection) spentSection.classList.add("hidden");

      if (data.outputs.length === 0) {
        list.innerHTML = `
          <div class="bg-[#161C24] border border-[rgba(145,158,171,0.16)] p-6 rounded-xl text-center space-y-1">
            <div class="text-xs font-semibold text-[#F9FAFB]">Sin Salidas Detectadas</div>
            <div class="text-xs text-[#919EAB]">No se encontraron salidas asociadas a esta clave de vista.</div>
          </div>`;
      } else {
        list.innerHTML = data.outputs.map(o => `
          <div class="bg-[#161C24] border border-[rgba(145,158,171,0.16)] p-3.5 rounded-xl flex flex-col sm:flex-row sm:items-center justify-between gap-3 text-xs">
            <div>
              <div class="font-bold text-[#00B8D9] mono text-sm">${o.amount_usdt}</div>
              <div class="text-[11px] text-[#919EAB] mono select-all truncate max-w-md mt-0.5" title="${o.destination_one_time}">
                Destino P: ${o.destination_one_time}
              </div>
            </div>
            <div class="flex items-center space-x-2 shrink-0">
              <button onclick="copyToTxForm('${o.destination_one_time}')" class="bg-[#8E33FF]/15 hover:bg-[#8E33FF]/25 text-[#8E33FF] border border-[#8E33FF]/30 px-3 py-1.5 rounded-lg text-xs font-semibold transition cursor-pointer">Usar para Transferir</button>
              <button onclick="copyToWithdrawForm('${o.destination_one_time}')" class="bg-[#00B8D9]/15 hover:bg-[#00B8D9]/25 text-[#00B8D9] border border-[#00B8D9]/30 px-3 py-1.5 rounded-lg text-xs font-semibold transition cursor-pointer">Usar para Retirar</button>
            </div>
          </div>
        `).join("");
      }

      showToast("Mostrando volumen histórico recibido (Modo Sólo Lectura)", "info");

    } else {
      // 2. MODO VERIFICACIÓN COMPLETA (Full Wallet - Con Spend Key)
      balLabel.textContent = "Saldo Neto Real Disponible:";
      balBadge.textContent = "VERIFICACIÓN COMPLETA (KEY IMAGES AUDITADAS)";
      balBadge.className = "text-[10px] px-2 py-0.5 rounded font-semibold uppercase bg-[#00A76F]/15 text-[#00A76F] border border-[#00A76F]/30";
      totalBal.className = "text-2xl font-bold mono mt-1 text-[#00A76F]";
      countLabel.textContent = "Salidas UTXO No Gastadas:";
      activeTitle.textContent = "Salidas UTXO Activas (Disponibles para Gastar):";

      modeBanner.className = "bg-[#00A76F]/10 border border-[#00A76F]/25 rounded-xl p-4 text-xs text-[#919EAB] flex items-start space-x-3";
      modeBanner.innerHTML = `
        <span class="p-1.5 bg-[#00A76F]/15 text-[#00A76F] rounded-lg shrink-0 mt-0.5">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>
        </span>
        <div class="leading-relaxed space-y-0.5">
          <div class="text-[#00A76F] font-bold text-xs">Billetera Completa Verificada Criptográficamente</div>
          <div>Cada una de tus salidas fue auditada calculando su Imagen de Clave (<code class="text-[#F9FAFB] font-mono">I = x·Hp(P)</code>) contra el ledger LMDB de la blockchain. Las salidas ya gastadas en transacciones previas han sido deducidas matemáticamente.</div>
        </div>
      `;

      if (data.outputs.length === 0) {
        list.innerHTML = `
          <div class="bg-[#161C24] border border-[rgba(145,158,171,0.16)] p-6 rounded-xl text-center space-y-1">
            <div class="text-xs font-semibold text-[#F9FAFB]">Sin Salidas Disponibles</div>
            <div class="text-xs text-[#919EAB]">No se encontraron UTXOs activos disponibles para gastar.</div>
          </div>`;
      } else {
        list.innerHTML = data.outputs.map(o => `
          <div class="bg-[#161C24] border border-[rgba(145,158,171,0.16)] p-3.5 rounded-xl flex flex-col sm:flex-row sm:items-center justify-between gap-3 text-xs">
            <div>
              <div class="font-bold text-[#00A76F] mono text-sm">${o.amount_usdt}</div>
              <div class="text-[11px] text-[#919EAB] mono select-all truncate max-w-md mt-0.5" title="${o.destination_one_time}">
                Destino P: ${o.destination_one_time}
              </div>
            </div>
            <div class="flex items-center space-x-2 shrink-0">
              <button onclick="copyToTxForm('${o.destination_one_time}')" class="bg-[#8E33FF]/15 hover:bg-[#8E33FF]/25 text-[#8E33FF] border border-[#8E33FF]/30 px-3 py-1.5 rounded-lg text-xs font-semibold transition cursor-pointer">Usar para Transferir</button>
              <button onclick="copyToWithdrawForm('${o.destination_one_time}')" class="bg-[#00B8D9]/15 hover:bg-[#00B8D9]/25 text-[#00B8D9] border border-[#00B8D9]/30 px-3 py-1.5 rounded-lg text-xs font-semibold transition cursor-pointer">Usar para Retirar</button>
            </div>
          </div>
        `).join("");
      }

      // Renderizar Salidas Gastadas
      if (spentSection && spentList) {
        if (data.spent_outputs && data.spent_outputs.length > 0) {
          spentSection.classList.remove("hidden");
          if (spentCount) spentCount.textContent = data.spent_outputs.length;
          spentList.innerHTML = data.spent_outputs.map(o => `
            <div class="bg-[#161C24]/60 border border-[rgba(145,158,171,0.12)] p-3 rounded-xl flex flex-col sm:flex-row sm:items-center justify-between gap-3 text-xs opacity-75">
              <div>
                <div class="flex items-center space-x-2">
                  <span class="line-through text-[#919EAB] mono text-sm">${o.amount_usdt}</span>
                  <span class="text-[10px] bg-[#FF5630]/15 text-[#FF5630] font-bold px-2 py-0.5 rounded border border-[#FF5630]/30 uppercase">CONSUMIDA (Key Image en Ledger)</span>
                </div>
                <div class="text-[11px] text-[#637381] mono select-all truncate max-w-md mt-0.5" title="${o.destination_one_time}">
                  Destino P: ${o.destination_one_time}
                </div>
              </div>
              <div class="text-[11px] text-[#919EAB] italic">Deducida del saldo neto</div>
            </div>
          `).join("");
        } else {
          spentSection.classList.add("hidden");
        }
      }

      showToast("Saldo neto verificado: " + data.total_balance_usdt + " disponibles", "success");
    }
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = `<svg class="w-4 h-4 text-[#161C24]" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z"></path></svg> <span>Escanear Fondos en Blockchain</span>`;
  }
}

function copyToTxForm(pubkey) {
  document.getElementById("tx-input-pub").value = pubkey;
  if (currentActiveWallet) {
    document.getElementById("tx-spend-priv").value = currentActiveWallet.spend_private_key;
    document.getElementById("tx-view-priv").value = currentActiveWallet.view_private_key;
  }
  switchTab("tab-transfer");
  showToast("UTXO asignado al formulario de transferencia RingCT", "info");
}

function copyToWithdrawForm(pubkey) {
  document.getElementById("withdraw-input-pub").value = pubkey;
  if (currentActiveWallet) {
    document.getElementById("withdraw-spend-priv").value = currentActiveWallet.spend_private_key;
    document.getElementById("withdraw-view-priv").value = currentActiveWallet.view_private_key;
  }
  switchTab("tab-vault");
  showToast("UTXO asignado al formulario de retiro con mezclador", "info");
}

// Execute Shielded Transfer (RingCT)
async function executeTransfer() {
  const btn = document.getElementById("btn-transfer");
  const spendPrivInput = document.getElementById("tx-spend-priv");
  const viewPrivInput = document.getElementById("tx-view-priv");
  const inputPubInput = document.getElementById("tx-input-pub");
  const recipientInput = document.getElementById("tx-recipient");
  const amountInput = document.getElementById("tx-amount");

  const spendPriv = spendPrivInput.value.trim();
  const viewPriv = viewPrivInput.value.trim();
  const inputPub = inputPubInput.value.trim();
  const recipient = recipientInput.value.trim();
  const amount = parseFloat(amountInput.value);

  let hasError = false;
  if (!spendPriv) { setInputError(spendPrivInput, true); hasError = true; }
  if (!viewPriv) { setInputError(viewPrivInput, true); hasError = true; }
  if (!inputPub) { setInputError(inputPubInput, true); hasError = true; }
  if (!recipient) { setInputError(recipientInput, true); hasError = true; }
  if (!amount || amount <= 0) { setInputError(amountInput, true); hasError = true; }

  if (hasError) {
    showToast("Completa todos los campos obligatorios de la transferencia", "error");
    return;
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Generando MLSAG y Minando...`;

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

    markUtxoSpentLocally(inputPub);
    showToast(`Transferencia confidencial minada en Bloque #${data.block_height}`, "success");
    fetchNodeData();
    fetchBlocks();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = `<svg class="w-4 h-4 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z"></path></svg> <span>Firmar con Anillo (MLSAG) y Enviar</span>`;
  }
}

// Execute Withdrawal with Tumbler
async function executeWithdrawal() {
  const btn = document.getElementById("btn-withdraw");
  const spendPrivInput = document.getElementById("withdraw-spend-priv");
  const viewPrivInput = document.getElementById("withdraw-view-priv");
  const inputPubInput = document.getElementById("withdraw-input-pub");
  const amountInput = document.getElementById("withdraw-amount");
  const destInput = document.getElementById("withdraw-dest");

  const spendPriv = spendPrivInput.value.trim();
  const viewPriv = viewPrivInput.value.trim();
  const inputPub = inputPubInput.value.trim();
  const amount = parseFloat(amountInput.value);
  const dest = destInput.value.trim();

  let hasError = false;
  if (!spendPriv) { setInputError(spendPrivInput, true); hasError = true; }
  if (!viewPriv) { setInputError(viewPrivInput, true); hasError = true; }
  if (!inputPub) { setInputError(inputPubInput, true); hasError = true; }
  if (!amount || amount <= 0) { setInputError(amountInput, true); hasError = true; }
  if (!dest) { setInputError(destInput, true); hasError = true; }

  if (hasError) {
    showToast("Completa todos los campos obligatorios para el retiro", "error");
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

    markUtxoSpentLocally(inputPub);
    renderTumblerRoutes(data);
    switchTab("tab-tumbler");
    showToast(`Retiro de ${data.net_usdt_tumbled} dispersado en ${data.total_micro_fragments} fragmentos`, "success");
    fetchNodeData();
    fetchBlocks();
  } catch (err) {
    showToast(err.message, "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = `<svg class="w-4 h-4 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 14l-7 7m0 0l-7-7m7 7V3"></path></svg> <span>Ejecutar Retiro y Mezclador</span>`;
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
  const destElem = document.getElementById("tumb-dest");
  if (destElem) {
    destElem.textContent = plan.destination || plan.destination_address || "Arbitrum L2";
  }

  const container = document.getElementById("tumb-routes-container");
  container.innerHTML = plan.routes.map(r => `
    <div class="bg-[#161C24] border border-[rgba(145,158,171,0.16)] p-4 rounded-xl space-y-2">
      <div class="flex items-center justify-between text-xs">
        <span class="font-bold text-[#F9FAFB]">Fragmento #${r.fragment_index}</span>
        <span class="mono text-[#00A76F] font-bold">${r.net_amount_usdt}</span>
      </div>
      <div class="text-[11px] text-[#919EAB] flex items-center justify-between border-t border-[rgba(145,158,171,0.12)] pt-2">
        <span>Ruta de saltos:</span>
        <span class="text-[#8E33FF] font-semibold">${r.hops_count} saltos efímeros programados</span>
      </div>
    </div>
  `).join("");
}

// Execute Treasury Fee Claim on Arbitrum Sepolia L2 with MetaMask
async function executeClaimFees() {
  const btn = document.getElementById("btn-claim-fees");
  const amountInput = document.getElementById("claim-fee-amount");
  const addrInput = document.getElementById("claim-fee-address");

  const amount = parseFloat(amountInput.value);
  const dest = addrInput.value.trim();

  let hasError = false;
  if (!amount || amount <= 0) { setInputError(amountInput, true); hasError = true; }
  if (!dest || !ethers.isAddress(dest)) { setInputError(addrInput, true); hasError = true; }

  if (hasError) {
    showToast("Ingresa un monto válido y una dirección Ethereum (0x...) válida", "error");
    return;
  }

  // 1. Asegurar que MetaMask esté conectado
  if (!web3Signer || !web3UserAddress) {
    await connectMetaMask();
    if (!web3Signer || !web3UserAddress) {
      showToast("Conecta MetaMask para autorizar el cobro on-chain en Arbitrum", "error");
      return;
    }
  }

  btn.disabled = true;
  btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-[#161C24] inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> <span>Confirmando en MetaMask...</span>`;

  try {
    const vaultContract = new ethers.Contract(VAULT_CONTRACT_ADDRESS, VAULT_ABI, web3Signer);

    // 2. Verificar que la cuenta conectada sea el Owner
    const contractOwner = await vaultContract.owner();
    if (contractOwner.toLowerCase() !== web3UserAddress.toLowerCase()) {
      throw new Error(`Solo el propietario del contrato (${contractOwner.slice(0, 6)}...${contractOwner.slice(-4)}) puede cobrar comisiones de tesorería.`);
    }

    const accumulatedOnChain = await vaultContract.accumulatedFees();
    const amountUnits = ethers.parseUnits(amount.toString(), 6);
    if (amountUnits > accumulatedOnChain) {
      throw new Error(`El monto solicitado (${amount} USDT) supera las comisiones disponibles on-chain (${ethers.formatUnits(accumulatedOnChain, 6)} USDT).`);
    }

    showToast("Transfiriendo comisiones de tesorería en Arbitrum Sepolia...", "info");
    const overrides = await getArbitrumTxOverrides(web3Provider);
    const tx = await vaultContract.claimTreasuryFees(dest, amountUnits, overrides);
    showToast(`Tx on-chain enviada: ${tx.hash.slice(0, 10)}... Esperando confirmación`, "info");

    btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-[#161C24] inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> <span>Minando en Arbitrum L2...</span>`;
    const receipt = await tx.wait();

    // 3. Sincronizar contabilidad local con el nodo C++
    try {
      await fetch("/api/v1/vault/claim-fees", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          amount_usdt: amount,
          treasury_address: dest
        })
      });
    } catch (syncErr) {
      console.warn("Aviso al sincronizar con nodo C++:", syncErr.message);
    }

    showToast(`¡Ganancias de ${amount.toFixed(2)} USDT transferidas exitosamente a ${dest.slice(0, 6)}...!`, "success");
    amountInput.value = "";
    fetchNodeData();
    await updateWeb3UI();
  } catch (err) {
    console.error("Error en cobro de tesorería:", err);
    showToast(err.reason || err.message || "Fallo al cobrar comisiones de tesorería", "error");
  } finally {
    btn.disabled = false;
    btn.innerHTML = `<svg class="w-4 h-4 text-[#161C24]" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M17 9V7a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2m2 4h10a2 2 0 002-2v-6a2 2 0 00-2-2H9a2 2 0 00-2 2v6a2 2 0 002 2zm7-5a2 2 0 11-4 0 2 2 0 014 0z"></path></svg> <span>Transferir Ganancias a Tesorería</span>`;
  }
}

// =================================================================
// WEB3 & METAMASK INTEGRATION (ARBITRUM SEPOLIA)
// =================================================================
const ARB_SEPOLIA_CHAIN_ID = 421614;
const ARB_SEPOLIA_HEX = "0x66eee";
const VAULT_CONTRACT_ADDRESS = "0x511A31987EF1019a41CBba658935515Dd64d2D18";
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
  "function getCirculatingBacking() external view returns (uint256)",
  "function claimTreasuryFees(address treasuryDestination, uint256 amount) external",
  "function owner() external view returns (address)"
];

let web3Provider = null;
let web3Signer = null;
let web3UserAddress = null;

// Connect MetaMask
async function connectMetaMask() {
  if (typeof window.ethereum === "undefined") {
    showToast("MetaMask no detectado. Instala MetaMask para interactuar en Arbitrum Sepolia.", "error");
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
    showToast("MetaMask conectado exitosamente a Arbitrum Sepolia", "success");
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
    statusBtn.className = "flex items-center space-x-2 bg-[#00A76F]/10 hover:bg-[#00A76F]/20 border border-[#00A76F]/40 px-4 py-2 rounded-xl text-xs font-semibold text-[#00A76F] transition shadow-card cursor-pointer font-mono";
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

    const withdrawDestInput = document.getElementById("withdraw-dest");
    if (withdrawDestInput && (!withdrawDestInput.value || withdrawDestInput.value.includes("0xColdStorage"))) {
      withdrawDestInput.value = web3UserAddress;
    }

    const treasuryDestInput = document.getElementById("claim-fee-address");
    if (treasuryDestInput && !treasuryDestInput.value) {
      treasuryDestInput.value = web3UserAddress;
    }

    // Consultar comisiones acumuladas directamente en la bóveda de Arbitrum L2
    try {
      const vaultContract = new ethers.Contract(VAULT_CONTRACT_ADDRESS, VAULT_ABI, web3Provider);
      const accFees = await vaultContract.accumulatedFees();
      const treasuryPoolDisplay = document.getElementById("treasury-pool-display");
      if (treasuryPoolDisplay) {
        treasuryPoolDisplay.textContent = `${parseFloat(ethers.formatUnits(accFees, 6)).toFixed(6)} USDT`;
      }
    } catch (contractErr) {
      console.warn("No se pudo leer accumulatedFees() on-chain:", contractErr);
    }
  } catch (e) {
    console.error("Error leyendo balances Web3:", e);
  }
}

function useMetaMaskAddressForWithdraw() {
  if (web3UserAddress) {
    const destInput = document.getElementById("withdraw-dest");
    if (destInput) {
      destInput.value = web3UserAddress;
      setInputError(destInput, false);
      showToast("Dirección MetaMask copiada al campo de retiro", "info");
    }
  } else {
    connectMetaMask().then(() => {
      if (web3UserAddress) {
        const destInput = document.getElementById("withdraw-dest");
        if (destInput) {
          destInput.value = web3UserAddress;
          setInputError(destInput, false);
          showToast("MetaMask conectado y dirección configurada para el retiro", "success");
        }
      }
    });
  }
}

function useMetaMaskAddressForTreasury() {
  if (web3UserAddress) {
    const destInput = document.getElementById("claim-fee-address");
    if (destInput) {
      destInput.value = web3UserAddress;
      setInputError(destInput, false);
      showToast("Dirección MetaMask configurada para tesorería", "info");
    }
  } else {
    connectMetaMask().then(() => {
      if (web3UserAddress) {
        const destInput = document.getElementById("claim-fee-address");
        if (destInput) {
          destInput.value = web3UserAddress;
          setInputError(destInput, false);
          showToast("MetaMask conectado y dirección de tesorería configurada", "success");
        }
      }
    });
  }
}

// Faucet: Mint 10,000 MockUSDT for testing on Arbitrum Sepolia
async function faucetMintUSDT() {
  if (!web3Signer || !web3UserAddress) {
    await connectMetaMask();
    if (!web3Signer || !web3UserAddress) return;
  }

  const btn = document.getElementById("btn-faucet-mint");
  const btnCompact = document.getElementById("btn-faucet-mint-compact");
  const originalHtml = btn ? btn.innerHTML : "";
  const originalCompactHtml = btnCompact ? btnCompact.innerHTML : "";

  if (btn) {
    btn.disabled = true;
    btn.innerHTML = `<svg class="animate-spin h-3 w-3 inline text-white" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> <span>Acuñando...</span>`;
  }
  if (btnCompact) {
    btnCompact.disabled = true;
    btnCompact.innerHTML = `<svg class="animate-spin h-3 w-3 inline text-[#00A76F]" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> <span>Acuñando...</span>`;
  }

  try {
    const usdtContract = new ethers.Contract(USDT_CONTRACT_ADDRESS, [
      "function mint(address to, uint256 amount) external"
    ], web3Signer);

    showToast("Acuñando 10,000 USDT de prueba en Arbitrum Sepolia...", "info");
    const amount = ethers.parseUnits("10000", 6);
    const overrides = await getArbitrumTxOverrides(web3Provider);
    const tx = await usdtContract.mint(web3UserAddress, amount, overrides);
    showToast(`Tx enviada (${tx.hash.slice(0, 10)}...). Esperando confirmación`, "info");
    await tx.wait();
    showToast("¡10,000 MockUSDT acuñados exitosamente en tu MetaMask!", "success");
    await updateWeb3UI();
  } catch (err) {
    console.error("Error al acuñar faucet USDT:", err);
    showToast(err.message || "Error al solicitar fondos a la faucet", "error");
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.innerHTML = originalHtml;
    }
    if (btnCompact) {
      btnCompact.disabled = false;
      btnCompact.innerHTML = originalCompactHtml;
    }
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

  const amountInput = document.getElementById("deposit-amount");
  const recipInput = document.getElementById("deposit-recipient");
  const grossVal = parseFloat(amountInput.value);
  let recipient = recipInput.value.trim();

  if (!grossVal || grossVal <= 0) {
    setInputError(amountInput, true);
    showToast("Ingresa un monto válido en USDT a depositar", "error");
    return;
  }

  // If no stealth recipient provided, generate one automatically
  if (!recipient) {
    await generateAndUseNewStealthAddress();
    recipient = recipInput.value.trim();
  }

  if (!recipient.startsWith("STX") || recipient.length !== 131) {
    setInputError(recipInput, true);
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
    btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> 1/2: Verificando / Aprobando USDT...`;
    const currentAllowance = await usdtContract.allowance(web3UserAddress, VAULT_CONTRACT_ADDRESS);

    if (currentAllowance < depositAmountUnits) {
      showToast("Confirma la aprobación de USDT en MetaMask...", "info");
      const approveOverrides = await getArbitrumTxOverrides(web3Provider);
      const approveTx = await usdtContract.approve(VAULT_CONTRACT_ADDRESS, depositAmountUnits, {
        ...approveOverrides,
        gasLimit: 120000n
      });
      btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Esperando confirmación de aprobación en Arbitrum...`;
      await approveTx.wait();
      showToast("¡USDT Aprobado con éxito! Ahora confirma el depósito...", "success");
    }

    // Step 2: Execute deposit
    btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> 2/2: Confirmando depósito en CryptoPegVault...`;
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

    btn.innerHTML = `<svg class="animate-spin -ml-1 mr-2 h-4 w-4 text-white inline" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path></svg> Minando bloque en Arbitrum Sepolia...`;
    showToast(`Tx enviada: ${depositTx.hash.slice(0, 14)}...`, "info");

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
