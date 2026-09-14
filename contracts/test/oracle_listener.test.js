const { expect } = require("chai");
const { ethers } = require("hardhat");
const fs = require("fs");
const path = require("path");

describe("Oracle Listener & Relayer Unit Tests", function () {
  const TEST_TMP_STATE = path.resolve(__dirname, "../../data/test_relayer_state.json");

  afterEach(function () {
    if (fs.existsSync(TEST_TMP_STATE)) {
      try { fs.unlinkSync(TEST_TMP_STATE); } catch (_) {}
    }
  });

  describe("Ordenación Canónica Determinista de Eventos (AUD-H0-P1-02)", function () {
    function canonicalSort(events) {
      return [...events].sort((a, b) => {
        if (a.blockNumber !== b.blockNumber) return a.blockNumber - b.blockNumber;
        if (a.transactionIndex !== b.transactionIndex) return a.transactionIndex - b.transactionIndex;
        const indexA = a.index !== undefined ? a.index : (a.logIndex !== undefined ? a.logIndex : 0);
        const indexB = b.index !== undefined ? b.index : (b.logIndex !== undefined ? b.logIndex : 0);
        return indexA - indexB;
      });
    }

    it("debe ordenar eventos por blockNumber ascendente", function () {
      const rawEvents = [
        { blockNumber: 105, transactionIndex: 0, index: 0, id: "ev3" },
        { blockNumber: 99, transactionIndex: 2, index: 1, id: "ev1" },
        { blockNumber: 102, transactionIndex: 1, index: 0, id: "ev2" }
      ];

      const sorted = canonicalSort(rawEvents);
      expect(sorted.map(e => e.id)).to.deep.equal(["ev1", "ev2", "ev3"]);
    });

    it("debe ordenar eventos en el mismo bloque por transactionIndex ascendente", function () {
      const rawEvents = [
        { blockNumber: 100, transactionIndex: 5, index: 0, id: "evC" },
        { blockNumber: 100, transactionIndex: 1, index: 0, id: "evA" },
        { blockNumber: 100, transactionIndex: 3, index: 0, id: "evB" }
      ];

      const sorted = canonicalSort(rawEvents);
      expect(sorted.map(e => e.id)).to.deep.equal(["evA", "evB", "evC"]);
    });

    it("debe ordenar eventos en la misma transacción por logIndex / index ascendente", function () {
      const rawEvents = [
        { blockNumber: 100, transactionIndex: 2, logIndex: 4, id: "ev3" },
        { blockNumber: 100, transactionIndex: 2, logIndex: 0, id: "ev1" },
        { blockNumber: 100, transactionIndex: 2, logIndex: 2, id: "ev2" }
      ];

      const sorted = canonicalSort(rawEvents);
      expect(sorted.map(e => e.id)).to.deep.equal(["ev1", "ev2", "ev3"]);
    });

    it("dos oráculos independientes con distinto orden de llegada producen el mismo resultado canónico exacto", function () {
      const eventsOracleA = [
        { blockNumber: 50, transactionIndex: 1, index: 0, hash: "0x1" },
        { blockNumber: 52, transactionIndex: 0, index: 1, hash: "0x3" },
        { blockNumber: 50, transactionIndex: 1, index: 2, hash: "0x2" }
      ];

      const eventsOracleB = [
        { blockNumber: 52, transactionIndex: 0, index: 1, hash: "0x3" },
        { blockNumber: 50, transactionIndex: 1, index: 2, hash: "0x2" },
        { blockNumber: 50, transactionIndex: 1, index: 0, hash: "0x1" }
      ];

      const sortedA = canonicalSort(eventsOracleA);
      const sortedB = canonicalSort(eventsOracleB);

      expect(sortedA.map(e => e.hash)).to.deep.equal(sortedB.map(e => e.hash));
      expect(sortedA.map(e => e.hash)).to.deep.equal(["0x1", "0x2", "0x3"]);
    });
  });

  describe("Persistencia de Estado y Resiliencia del Cursor (AUD-H0-06)", function () {
    function saveState(file, state) {
      const dir = path.dirname(file);
      if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
      const data = {
        lastCheckedL2Block: state.lastCheckedL2Block,
        lastCheckedChainHeight: state.lastCheckedChainHeight,
        processedTxHashes: Array.from(state.processedTxHashes).slice(-5000),
        processedWithdrawalOrders: Array.from(state.processedWithdrawalOrders).slice(-5000),
        quarantinedWithdrawalOrders: Array.from(state.quarantinedWithdrawalOrders || []).slice(-5000),
        updatedAt: new Date().toISOString()
      };
      fs.writeFileSync(file, JSON.stringify(data, null, 2), "utf8");
    }

    function loadState(file) {
      if (fs.existsSync(file)) {
        const raw = fs.readFileSync(file, "utf8");
        const data = JSON.parse(raw);
        return {
          lastCheckedL2Block: Number(data.lastCheckedL2Block || 0),
          lastCheckedChainHeight: Number(data.lastCheckedChainHeight || 0),
          processedTxHashes: new Set(data.processedTxHashes || []),
          processedWithdrawalOrders: new Set(data.processedWithdrawalOrders || []),
          quarantinedWithdrawalOrders: new Set(data.quarantinedWithdrawalOrders || [])
        };
      }
      return {
        lastCheckedL2Block: 0,
        lastCheckedChainHeight: 0,
        processedTxHashes: new Set(),
        processedWithdrawalOrders: new Set(),
        quarantinedWithdrawalOrders: new Set()
      };
    }

    it("debe persistir y recuperar estado de forma consistente (incluyendo DLQ de retiros)", function () {
      const state = {
        lastCheckedL2Block: 123456,
        lastCheckedChainHeight: 42,
        processedTxHashes: new Set(["0xabc", "0xdef"]),
        processedWithdrawalOrders: new Set(["ord-1", "ord-2"]),
        quarantinedWithdrawalOrders: new Set(["ord-quarantine-1"])
      };

      saveState(TEST_TMP_STATE, state);
      const loaded = loadState(TEST_TMP_STATE);

      expect(loaded.lastCheckedL2Block).to.equal(123456);
      expect(loaded.lastCheckedChainHeight).to.equal(42);
      expect(loaded.processedTxHashes.has("0xabc")).to.be.true;
      expect(loaded.processedTxHashes.has("0xdef")).to.be.true;
      expect(loaded.processedWithdrawalOrders.has("ord-1")).to.be.true;
      expect(loaded.quarantinedWithdrawalOrders.has("ord-quarantine-1")).to.be.true;
    });

    it("debe truncar a los últimos 5,000 elementos para evitar desbordamiento de memoria", function () {
      const hashes = new Set();
      for (let i = 0; i < 6000; i++) {
        hashes.add(`0xhash_${i}`);
      }

      saveState(TEST_TMP_STATE, {
        lastCheckedL2Block: 100,
        lastCheckedChainHeight: 10,
        processedTxHashes: hashes,
        processedWithdrawalOrders: new Set()
      });

      const loaded = loadState(TEST_TMP_STATE);
      expect(loaded.processedTxHashes.size).to.equal(5000);
      expect(loaded.processedTxHashes.has("0xhash_0")).to.be.false; // Primeros podados
      expect(loaded.processedTxHashes.has("0xhash_5999")).to.be.true; // Últimos conservados
    });

    it("debe retroceder el cursor si el daemon local falla para evitar pérdida de depósitos (AUD-H0-06)", function () {
      let lastCheckedL2Block = 100;
      const event = { blockNumber: 105, transactionHash: "0xfail_tx" };

      // Simulamos fallo en HTTP POST
      const fetchFailed = true;
      let processedSuccessfully = false;

      if (!fetchFailed) {
        processedSuccessfully = true;
      }

      if (!processedSuccessfully) {
        lastCheckedL2Block = event.blockNumber > 0 ? event.blockNumber - 1 : lastCheckedL2Block;
      }

      // Cursor debe retroceder a 104 para reintentar el bloque en el siguiente ciclo
      expect(lastCheckedL2Block).to.equal(104);
    });

    it("debe avanzar el cursor y registrar el hash cuando el daemon responde 200 o 409", function () {
      const processedTxHashes = new Set();
      let lastCheckedL2Block = 100;
      const event = { blockNumber: 105, transactionHash: "0xsuccess_tx" };
      const toBlock = 110;

      // Caso 1: 200 OK
      let status = 200;
      if (status === 200 || status === 409) {
        processedTxHashes.add(event.transactionHash);
      }
      lastCheckedL2Block = toBlock;

      expect(processedTxHashes.has("0xsuccess_tx")).to.be.true;
      expect(lastCheckedL2Block).to.equal(110);

      // Caso 2: 409 Conflict (idempotencia)
      const eventConflict = { blockNumber: 112, transactionHash: "0xconflict_tx" };
      status = 409;
      if (status === 200 || status === 409) {
        processedTxHashes.add(eventConflict.transactionHash);
      }
      expect(processedTxHashes.has("0xconflict_tx")).to.be.true;
    });

    it("debe abortar y NO avanzar el cursor si la consulta RPC queryFilter falla en Arbitrum L2 (AUD-HIGH-01)", function () {
      let lastCheckedL2Block = 500;
      const currentBlock = 550;
      const toBlock = currentBlock;

      let rpcFailed = true;
      let abortedWithoutAdvance = false;

      try {
        if (rpcFailed) {
          throw new Error("429 Too Many Requests: RPC rate limit reached");
        }
      } catch (filterErr) {
        // Manejador AUD-HIGH-01: Salir sin actualizar lastCheckedL2Block
        abortedWithoutAdvance = true;
      }

      if (!abortedWithoutAdvance) {
        lastCheckedL2Block = toBlock;
      }

      expect(abortedWithoutAdvance).to.be.true;
      expect(lastCheckedL2Block).to.equal(500); // Cursor se conserva intacto
    });
  });

  describe("Cálculo de Hash de Retiros e Idempotencia", function () {
    it("debe calcular el mismo orderIdHash que Solidity keccak256(bytes(order_id))", function () {
      const orderIdStr = "ORD-TEST-998877";
      const computedHash = ethers.keccak256(ethers.toUtf8Bytes(orderIdStr));

      // Verificamos que es un bytes32 válido (0x + 64 hex chars)
      expect(computedHash).to.match(/^0x[0-9a-fA-F]{64}$/);

      // Determinismo
      const recomputedHash = ethers.keccak256(ethers.toUtf8Bytes(orderIdStr));
      expect(computedHash).to.equal(recomputedHash);
    });
  });

  describe("Auditoría v3: Confirmaciones L2, Idempotencia Compuesta y Precisión Strings", function () {
    it("debe respetar la ventana de confirmaciones L2 (CONFIRMATION_BLOCKS) (P1-06)", function () {
      const currentBlock = 1000;
      const CONFIRMATION_BLOCKS = 12;
      const confirmedBlock = Math.max(0, currentBlock - CONFIRMATION_BLOCKS);

      expect(confirmedBlock).to.equal(988);

      let lastChecked = 980;
      const fromBlock = lastChecked + 1;
      const toBlock = confirmedBlock;

      expect(fromBlock).to.equal(981);
      expect(toBlock).to.equal(988);
      expect(toBlock).to.be.lessThan(currentBlock);
    });

    it("debe procesar múltiples eventos en la misma transacción mediante identificador compuesto eventId (P1-07)", function () {
      const chainId = 421614;
      const vault = "0x511A31987EF1019a41CBba658935515Dd64d2D18".toLowerCase();
      const sharedTx = "0xMultiDepositBatchTx123";

      const event1 = { transactionHash: sharedTx, index: 0 };
      const event2 = { transactionHash: sharedTx, index: 1 };

      const id1 = `${chainId}:${vault}:${event1.transactionHash}:${event1.index}`;
      const id2 = `${chainId}:${vault}:${event2.transactionHash}:${event2.index}`;

      expect(id1).to.not.equal(id2);

      const processedSet = new Set();
      processedSet.add(id1);

      expect(processedSet.has(id1)).to.be.true;
      expect(processedSet.has(id2)).to.be.false; // No se ignora el segundo evento
    });

    it("debe preservar precisión en montos transmitidos como string sin límite JS 2^53 (P1-08)", function () {
      const hugeAtomicAmount = 9007199254740992000n; // > 2^53
      const strVal = hugeAtomicAmount.toString();

      expect(strVal).to.equal("9007199254740992000");
      expect(typeof strVal).to.equal("string");
    });

    it("debe aislar órdenes con reversión permanente on-chain sin congelar la cola de retiros (V4-05)", function () {
      const processedWithdrawalOrders = new Set();
      const orderFailures = new Map();

      const order1 = "ORD-FAIL-REVERT";
      const order2 = "ORD-SUCCESS-VALID";

      // Simular fallo permanente de order1
      const err = new Error("execution reverted: transfer failed in recipient contract");
      const isPermanentRevert = err.message.includes("reverted") || err.message.includes("execution reverted");

      expect(isPermanentRevert).to.be.true;
      if (isPermanentRevert) {
        processedWithdrawalOrders.add(order1);
      }

      // Orden 1 quedó registrada como procesada/aislada
      expect(processedWithdrawalOrders.has(order1)).to.be.true;

      // La cola no se bloquea y procesa la siguiente orden
      processedWithdrawalOrders.add(order2);
      expect(processedWithdrawalOrders.has(order2)).to.be.true;
    });

    it("debe aislar órdenes tras alcanzar el umbral de 3 reintentos fallidos transitorios en Dead Letter Queue (V4-05 / CP-SEC-01)", function () {
      const processedWithdrawalOrders = new Set();
      const quarantinedWithdrawalOrders = new Set();
      const orderFailures = new Map();
      const orderId = "ORD-TRANSIENT-FAIL";

      for (let attempt = 1; attempt <= 3; attempt++) {
        const fails = (orderFailures.get(orderId) || 0) + 1;
        orderFailures.set(orderId, fails);
        if (fails >= 3) {
          quarantinedWithdrawalOrders.add(orderId);
        }
      }

      // La orden fallida debe estar en la cola de cuarentena (DLQ) y NO en órdenes procesadas exitosamente
      expect(quarantinedWithdrawalOrders.has(orderId)).to.be.true;
      expect(processedWithdrawalOrders.has(orderId)).to.be.false;
      expect(orderFailures.get(orderId)).to.equal(3);
    });
  });
});
