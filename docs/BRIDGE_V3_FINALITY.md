# CryptoPeg Bridge V3 — quorum finality

## Purpose

Bridge V3 closes the two P0 bridge findings from the 2026-09-14 adversarial review:

1. a deep private-chain fork must not be able to release USDT from two competing histories;
2. compromise of one ECDSA validator key must not be enough to release USDT from the Vault.

`CryptoPegVaultV3` is a new deployment. V2 remains legacy and MUST NOT be treated as fixed merely because V3 exists in the repository.

## Security model

- At least 3 bridge validators are required.
- The threshold is at least 2 and must be a strict majority (`M > N/2`).
- Validator membership is immutable for a V3 deployment. The owner cannot replace the set with one attacker-controlled key.
- Validators sign EIP-712 checkpoint certificates.
- A checkpoint can only advance and must reference the previous on-chain finalized checkpoint hash.
- A withdrawal is accepted only when it is signed by M-of-N validators and is bound to the latest finalized checkpoint.
- The transaction submitter/relayer is not trusted with withdrawal authority.

## Independent cosigners are mandatory

Do not place all validator keys in one process, one VPS, one Docker secret store, or one operator account.

Run `contracts/scripts/validator_cosigner_v3.js` independently for each validator. Each cosigner:

1. queries its own CryptoPeg node;
2. checks that the node contains the currently finalized on-chain checkpoint;
3. requires the proposed new checkpoint to be sufficiently buried;
4. checks the exact withdrawal is present in its local finalized block;
5. signs only after those checks pass.

The aggregator `withdrawal_relayer_v3.js` holds only a gas-paying relayer key. It requests signatures from the independent cosigners and cannot fabricate a quorum itself.

## Why this blocks the deep-fork release scenario

Assume the private chain splits into fork A and fork B. A checkpoint from fork A is finalized on Arbitrum with M-of-N signatures. After that:

- the on-chain V3 Vault stores fork A's checkpoint hash;
- a cosigner on fork B sees that its local chain does not contain the finalized checkpoint and refuses to sign withdrawals;
- a stale or competing checkpoint cannot be used by `withdraw`, because the withdrawal must reference the current on-chain checkpoint;
- one compromised validator cannot create either a checkpoint or a withdrawal certificate.

The security assumption is therefore explicit: an attacker must compromise or induce equivocation from a quorum of bridge validators, not merely one relayer or one validator key.

## Deployment sequence

1. Deploy `CryptoPegVaultV3` with at least 3 independently controlled validator addresses and a strict-majority threshold.
2. Run one independent CryptoPeg node + V3 cosigner per validator/operator.
3. Give each cosigner a different `COSIGNER_AUTH_TOKEN` and a different `BRIDGE_VALIDATOR_PRIVATE_KEY`.
4. Configure the aggregator with `V3_COSIGNERS_JSON`; the aggregator key is only a gas payer.
5. Exercise checkpoint finalization and withdrawal on Arbitrum Sepolia.
6. Simulate a deep fork: at least one cosigner must reject a checkpoint/withdrawal whose local chain does not contain the current finalized checkpoint.
7. Only after the adversarial tests pass should liquidity be migrated from V2.

## Required environment variables

### Cosigner

- `ARBITRUM_SEPOLIA_RPC_URL`
- `BRIDGE_V3_VAULT_ADDRESS`
- `NODE_DAEMON_URL`
- `BRIDGE_VALIDATOR_PRIVATE_KEY`
- `COSIGNER_AUTH_TOKEN`
- `COSIGNER_PORT`
- `CONFIDENTIAL_CHAIN_CONFIRMATIONS`

### Relayer/aggregator

- `ARBITRUM_SEPOLIA_RPC_URL`
- `BRIDGE_V3_VAULT_ADDRESS`
- `NODE_DAEMON_URL`
- `V3_RELAYER_PRIVATE_KEY`
- `V3_COSIGNERS_JSON`, for example:

```json
[
  {"url":"https://validator-a.example","token":"secret-a"},
  {"url":"https://validator-b.example","token":"secret-b"},
  {"url":"https://validator-c.example","token":"secret-c"}
]
```

Never commit the real tokens or private keys.

## Migration warning

The currently deployed V2 contract is still a single-signer bridge. V3 source code does not retroactively secure V2 funds. The production/mainnet readiness decision must remain **NO** until V3 is deployed, independently operated, tested under fork conditions, and the final adversarial audit passes.
