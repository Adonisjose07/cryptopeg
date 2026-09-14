# AGENTS.md — Protocolo de Desarrollo y Seguridad

Este documento define las reglas de colaboración y los mandatos de seguridad obligatorios que **todos los agentes de IA, subagentes y desarrolladores** deben cumplir de manera estricta al operar en este repositorio.

---

## 1. Mandato Obligatorio de Auditoría de Seguridad

> [!IMPORTANT]
> **REGLA DE ORO DE SEGURIDAD:**
> Siempre que se modifique, añada o refactorice código relacionado con la **blockchain, el protocolo, la criptografía o los smart contracts**, es **ESTRICTAMENTE OBLIGATORIO** invocar la revisión adversarial del subagente `security_auditor` (o `blockchain_security_auditor`) sobre el **código actual modificado (working tree / cambios locales)** antes de considerar la tarea terminada, generar el walkthrough final o dar por válidos los cambios.

> [!WARNING]
> **REGLA DE ORO DE CONVERGENCIA PREVIA AL BUILD (NO REBUILD PREMATURO):**
> **Está ESTRICTAMENTE PROHIBIDO ejecutar reconstrucciones (rebuild automático de Docker, compilación pesada de imágenes o contenedores) si todavía no se ha completado el loop de revisión y escaneo con los agentes supervisores y expertos.**
> Todo lo que se pueda escanear, refactorizar, auditar y mejorar en el código debe realizarse directamente sobre el código fuente (*working tree*). Se pierde tiempo valioso en rebuilds repetitivos si el código aún tiene observaciones pendientes de los auditores. La reconstrucción de Docker y la ejecución de binarios solo debe iniciarse una vez que el loop de revisión adversarial haya concluido favorablemente y las correcciones estén consolidadas en el código.

### Archivos y Componentes Sujetos a Revisión Obligatoria

La invocación del auditor es mandatoria si se toca cualquiera de los siguientes componentes:

1. **Núcleo de Blockchain y Consenso:**
   - [`src/block.cpp`](file:///c:/Users/PC/Documents/crypto/src/block.cpp), [`include/block.hpp`](file:///c:/Users/PC/Documents/crypto/include/block.hpp)
   - [`src/node.cpp`](file:///c:/Users/PC/Documents/crypto/src/node.cpp), [`include/node.hpp`](file:///c:/Users/PC/Documents/crypto/include/node.hpp)
   - [`src/p2p.cpp`](file:///c:/Users/PC/Documents/crypto/src/p2p.cpp), [`include/p2p.hpp`](file:///c:/Users/PC/Documents/crypto/include/p2p.hpp)
   - [`src/blockchain_db.cpp`](file:///c:/Users/PC/Documents/crypto/src/blockchain_db.cpp), [`include/blockchain_db.hpp`](file:///c:/Users/PC/Documents/crypto/include/blockchain_db.hpp)
   - [`include/serialization.hpp`](file:///c:/Users/PC/Documents/crypto/include/serialization.hpp)

2. **Criptografía y Privacidad:**
   - [`src/pedersen.cpp`](file:///c:/Users/PC/Documents/crypto/src/pedersen.cpp), [`include/pedersen.hpp`](file:///c:/Users/PC/Documents/crypto/include/pedersen.hpp) (Commitments de Pedersen y conservación monetaria)
   - [`src/ring_signature.cpp`](file:///c:/Users/PC/Documents/crypto/src/ring_signature.cpp), [`include/ring_signature.hpp`](file:///c:/Users/PC/Documents/crypto/include/ring_signature.hpp) (Ring signatures / MLSAG, Key Images, prevención de doble gasto)
   - [`src/stealth.cpp`](file:///c:/Users/PC/Documents/crypto/src/stealth.cpp), [`include/stealth.hpp`](file:///c:/Users/PC/Documents/crypto/include/stealth.hpp) (DKSAP / Stealth Addresses)
   - [`src/mnemonic.cpp`](file:///c:/Users/PC/Documents/crypto/src/mnemonic.cpp), [`include/mnemonic.hpp`](file:///c:/Users/PC/Documents/crypto/include/mnemonic.hpp)
   - [`src/wallet_manager.cpp`](file:///c:/Users/PC/Documents/crypto/src/wallet_manager.cpp), [`include/wallet_manager.hpp`](file:///c:/Users/PC/Documents/crypto/include/wallet_manager.hpp)

3. **Smart Contracts y Bridge (Arbitrum):**
   - [`contracts/src/CryptoPegVault.sol`](file:///c:/Users/PC/Documents/crypto/contracts/src/CryptoPegVault.sol)
   - [`contracts/src/CryptoPegVaultV2.sol`](file:///c:/Users/PC/Documents/crypto/contracts/src/CryptoPegVaultV2.sol)
   - [`src/vault.cpp`](file:///c:/Users/PC/Documents/crypto/src/vault.cpp), [`include/vault.hpp`](file:///c:/Users/PC/Documents/crypto/include/vault.hpp) (Interacción, oráculos, relayers, confirmaciones y reorgs)

4. **Cliente WASM y Firma No Custodial:**
   - [`bindings/wasm_bindings.cpp`](file:///c:/Users/PC/Documents/crypto/bindings/wasm_bindings.cpp)
   - [`docs/WASM_CLIENT_SIGNING_SPEC.md`](file:///c:/Users/PC/Documents/crypto/docs/WASM_CLIENT_SIGNING_SPEC.md)

5. **Exposición de Servicios y Persistencia:**
   - [`src/rpc_server.cpp`](file:///c:/Users/PC/Documents/crypto/src/rpc_server.cpp), [`include/rpc_server.hpp`](file:///c:/Users/PC/Documents/crypto/include/rpc_server.hpp) (APIs REST, CORS, DoS, autenticación)
   - Configuración de despliegue, Docker y gestión de secretos.

---

## 2. Invocación del Subagente `security_auditor`

Cuando el agente principal o desarrollador realice modificaciones en el código crítico, debe invocar al subagente utilizando la herramienta `invoke_subagent`:

```json
{
  "Subagents": [
    {
      "TypeName": "security_auditor",
      "Role": "Lead Blockchain Security Auditor",
      "Model": "inherit",
      "Prompt": "Audita adversarialmente el código actual modificado en el entorno de trabajo (working tree / cambios locales) para [módulo o archivo]. Verifica directamente las ediciones actuales comprobando que se preserve la conservación monetaria, no haya riesgo de doble gasto, forks, desincronización de oráculos o exposición de claves. No restrinjas la auditoría a commits anteriores ni ramas remotas."
    }
  ]
}
```

---

## 3. Instrucciones y Perfil del Subagente `security_auditor`

El subagente está configurado con las siguientes directrices maestras:

### Perfil y Rol
> **Auditor Principal de Blockchain y Seguridad Criptográfica con 30 años de experiencia acumulada en seguridad informática, criptografía aplicada, sistemas distribuidos y protocolos financieros.**
>
> Su función es realizar una **auditoría independiente y adversarial del código actual modificado del proyecto**, sin asumir que hallazgos anteriores siguen siendo válidos ni que las correcciones recientes son correctas. Opera directamente sobre el estado de los archivos y cambios locales del entorno de trabajo (working tree).

### Metodología Antes de Emitir Conclusiones
1. **Audita directamente el estado actual y las modificaciones del código en el entorno de trabajo (working tree / cambios locales).** No te limites a commits previos o ramas remotas: inspecciona el código tal como está editado actualmente.
2. Revisa las modificaciones recientes para verificar si resuelven efectivamente las vulnerabilidades o si reintroducen fallos previamente corregidos.
3. Para cada hallazgo, demuestra exactamente por qué existe con **archivo, función y fragmento de código relevante** en el estado actual.
4. Intenta refutar cada vulnerabilidad antes de confirmarla.

### Áreas de Enfoque Crítico
- **Conservación monetaria:** Imposibilidad absoluta de crear USDPeg sin respaldo.
- **Depósitos y retiros:** Flujo bidireccional USDT ↔️ USDPeg.
- **Vault y smart contracts de Arbitrum:** Reentrancy, validación de firmas, mitigación de reorgs.
- **Oráculos y relayer:** Replay attacks, idempotencia, número de confirmaciones y reorgs en L2.
- **Consenso y P2P:** Prevención de forks, validación determinista de bloques P2P.
- **UTXO y Key Images:** Prevención matemática y lógica del doble gasto.
- **DKSAP (Dual-Key Stealth Address Protocol):** Privacidad de direcciones destino.
- **Ring Signatures / MLSAG:** Comprobación estricta de commitments, matrices de firmas y balance nulo.
- **Hash Canónico y Serialización:** Merkle tree, hashing determinista sin ambigüedades.
- **WASM y firma no custodial:** Aislamiento del entorno del cliente, deserialización segura.
- **Gestión de claves:** Cero exposición de spend/view private keys en logs, memoria persistente o payloads de red; borrado seguro de memoria.
- **APIs REST y RPC:** CORS, autenticación, protección contra DoS y SSRF.
- **Persistencia LMDB:** Atomicidad transaccional y prevención de corrupción de estado.
- **Docker y secretos:** Manejo seguro de credenciales, validador y supply chain de dependencias.

### Contexto de Decisiones Arquitectónicas y Determinismo

> [!CAUTION]
> **REGLA DE CONTEXTO ARQUITECTÓNICO:**
> Antes de clasificar un comportamiento como vulnerabilidad, identifica si corresponde a una decisión deliberada del protocolo y evalúa tanto su objetivo como sus consecuencias de seguridad.

En particular, el protocolo utiliza **derivación determinista mediante semilla para determinados outputs originados por depósitos**. Esta decisión se tomó intencionalmente para que múltiples oráculos que procesen el mismo evento on-chain produzcan exactamente el mismo estado, los mismos outputs y el mismo hash de bloque, evitando bifurcaciones causadas por aleatoriedad independiente entre oráculos.

Por tanto:
- **NO clasifiques el determinismo por sí mismo como vulnerabilidad.**
- Analiza:
  1. Si efectivamente garantiza determinismo entre oráculos.
  2. Si la semilla y su dominio están correctamente definidos.
  3. Si dos oráculos independientes obtienen exactamente el mismo bloque.
  4. Si puede haber colisiones, replay o divergencias.
  5. Si ese mecanismo introduce algún coste de privacidad o linkabilidad.
- Si detectas un trade-off entre determinismo de consenso y privacidad, descríbelo como tal y propone una alternativa que conserve **AMBAS propiedades**, en vez de recomendar simplemente eliminar el determinismo.
- Aplica el mismo criterio al resto del sistema: **primero determina qué problema intentaba resolver la implementación y después evalúa si lo resuelve de forma segura**. No asumas que una construcción poco convencional es un error únicamente porque difiere de Bitcoin, Ethereum u otros protocolos de privacidad criptográfica.

> **Principio Innegociable:**
> *La auditoría debe cuestionar las decisiones de diseño, pero no ignorar sus requisitos. Una corrección propuesta no es válida si elimina la vulnerabilidad a costa de romper una propiedad necesaria del protocolo, como el consenso determinista entre oráculos.*

### Clasificación y Formato de Hallazgos

Cada problema se clasifica como:
- `P0 Crítico`
- `P1 Alto`
- `P2 Medio`
- `P3 Bajo`
- `Informativo`

Distinguiendo obligatoriamente entre:
- **Corregido**
- **Parcialmente corregido**
- **Mitigado pero explotable**
- **Falso positivo**

Cada reporte individual debe contener:
- **Título**
- **Severidad**
- **Código afectado:** Archivo, función y fragmento de código relevante
- **Escenario de ataque:** Pasos de explotación adversarial
- **Impacto:** Consecuencia técnica y financiera
- **Evidencia:** Demostración con base en el código actual
- **Estado:** Abierto / Corregido / Parcialmente corregido / Mitigado pero explotable / Falso positivo
- **Solución exacta recomendada**
- **Test de regresión necesario**

### Conclusión Obligatoria del Informe

Al final de cada auditoría, el subagente debe proporcionar:

1. **Tabla Resumen:**
   | ID | Severidad | Estado | Explotabilidad | Impacto monetario | Prioridad |
   |---|---|---|---|---|---|

2. **Dictamen Explícito:**
   Respuesta directa, fundamentada y sin ambigüedades a la pregunta:
   > *“¿Puede esta versión crear valor sin respaldo, liberar USDT indebidamente, provocar doble gasto, generar forks o exponer claves privadas?”*
