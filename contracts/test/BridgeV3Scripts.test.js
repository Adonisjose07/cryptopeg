const { expect } = require("chai");
const { execFileSync } = require("child_process");
const path = require("path");

describe("Bridge V3 operational scripts", function () {
  const scripts = [
    "deploy_v3.js",
    "validator_cosigner_v3.js",
    "withdrawal_relayer_v3.js",
  ];

  for (const script of scripts) {
    it(`parses ${script} with Node.js`, function () {
      const fullPath = path.resolve(__dirname, "../scripts", script);
      expect(() => execFileSync(process.execPath, ["--check", fullPath], { stdio: "pipe" })).to.not.throw();
    });
  }
});
