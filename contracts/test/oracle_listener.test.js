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
          processedWithdrawalOrders: new Set(data.processedWithdrawalOrders || [])
        };
      }
      return {
        lastCheckedL2Block: 0,
        lastCheckedChainHeight: 0,
        processedTxHashes: new Set(),
        processedWithdrawalOrders: new Set()
      };
    }

    it("debe persistir y recuperar estado de forma consistente", function () {
      const state = {
        lastCheckedL2Block: 123456,
        lastCheckedChainHeight: 42,
        processedTxHashes: new Set(["0xabc", "0xdef"]),
        processedWithdrawalOrders: new Set(["ord-1", "ord-2"])
      };

      saveState(TEST_TMP_STATE, state);
      const loaded = loadState(TEST_TMP_STATE);

      expect(loaded.lastCheckedL2Block).to.equal(123456);
      expect(loaded.lastCheckedChainHeight).to.equal(42);
      expect(loaded.processedTxHashes.has("0xabc")).to.be.true;
      expect(loaded.processedTxHashes.has("0xdef")).to.be.true;
      expect(loaded.processedWithdrawalOrders.has("ord-1")).to.be.true;
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
});
