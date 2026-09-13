const { expect } = require("chai");
const { ethers } = require("hardhat");

describe("MockUSDT Unit Tests", function () {
  let usdt, owner, user;

  beforeEach(async function () {
    [owner, user] = await ethers.getSigners();
    const MockUSDT = await ethers.getContractFactory("MockUSDT");
    usdt = await MockUSDT.deploy();
    await usdt.waitForDeployment();
  });

  it("debe tener nombre, símbolo y 6 decimales", async function () {
    expect(await usdt.name()).to.equal("Tether USD");
    expect(await usdt.symbol()).to.equal("USDT");
    expect(await usdt.decimals()).to.equal(6);
  });

  it("debe acuñar 10,000,000 USDT al deployer", async function () {
    const balance = await usdt.balanceOf(owner.address);
    expect(balance).to.equal(ethers.parseUnits("10000000", 6));
  });

  it("debe permitir mint público a cualquier cuenta", async function () {
    const mintAmount = ethers.parseUnits("5000", 6);
    await usdt.connect(user).mint(user.address, mintAmount);
    expect(await usdt.balanceOf(user.address)).to.equal(mintAmount);
  });
});
