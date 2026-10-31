// Optional browser regression suite; requires Node.js, Playwright, and Chromium.
// No test dependencies are loaded by the actual frontend.
const assert = require("node:assert/strict");
const http = require("node:http");
const { readFileSync, mkdirSync } = require("node:fs");
const path = require("node:path");
const { chromium } = require("playwright");

async function main() {
  const files = new Map([
    ["/", ["index.html", "text/html"]],
    ["/styles.css", ["styles.css", "text/css"]],
    ["/app.js", ["app.js", "text/javascript"]],
  ]);
  const server = http.createServer((request, response) => {
    const file = files.get(request.url);
    if (!file) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, { "Content-Type": file[1] });
    response.end(readFileSync(path.join(__dirname, "..", file[0])));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  let browser;

  try {
    browser = await chromium.launch({ headless: true });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    const errors = [];
    const methods = [];
    page.on("pageerror", (error) => errors.push(error.message));
    page.on("request", (request) => methods.push(request.method()));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.locator("#login-password").fill("demo-only");
    await page.getByRole("button", { name: "Enter your gallery" }).click();
    assert.equal(await page.locator("#collection-pagination").isVisible(), false);

    // Tiny processed images make large-collection tests inexpensive. Use real
    // browser blobs and the production renderer, not a separate pagination model.
    async function seed(count) {
      await page.evaluate(async (count) => {
        pauseSlideshow();
        for (const artwork of collection.splice(0)) URL.revokeObjectURL(artwork.url);
        const canvas = document.createElement("canvas");
        canvas.width = 60;
        canvas.height = 40;
        const context = canvas.getContext("2d");
        context.fillStyle = "#52745a";
        context.fillRect(0, 0, 60, 40);
        const blob = await new Promise((resolve) => canvas.toBlob(resolve));
        for (let id = 1; id <= count; id++) {
          collection.push({
            id,
            name: `Artwork ${id}`,
            treatment: "Nearest color",
            placement: "Fit to frame",
            url: URL.createObjectURL(blob),
          });
        }
        nextArtworkId = count + 1;
        selectedArtworkId = count ? 1 : null;
        collectionPage = 1;
        renderCollection();
      }, count);
    }

    async function assertPage(current, total, artworks, visible) {
      assert.equal(
        await page.locator("#pagination-summary").innerText(),
        `Page ${current} of ${total} · ${artworks} artwork${artworks === 1 ? "" : "s"}`
      );
      assert.equal(await page.locator(".collection-item").count(), visible);
      assert.equal(await page.locator("#page-jump").inputValue(), String(current));
      assert.equal(await page.locator("#page-jump").getAttribute("max"), String(total));
      assert.equal(await page.locator("#page-first").isDisabled(), current === 1);
      assert.equal(await page.locator("#page-previous").isDisabled(), current === 1);
      assert.equal(await page.locator("#page-next").isDisabled(), current === total);
      assert.equal(await page.locator("#page-last").isDisabled(), current === total);
      assert.equal(await page.locator("#page-numbers [aria-current='page']").count(), 1);
    }

    async function jump(value, enter = false) {
      await page.locator("#page-jump").fill(String(value));
      if (enter) await page.locator("#page-jump").press("Enter");
      else await page.locator("#page-jump-form button").click();
    }

    for (const count of [0, 1, 23, 24, 25, 48, 49]) {
      await seed(count);
      assert.equal(await page.locator(".collection-item").count(), Math.min(24, count));
      assert.equal(await page.locator("#collection-pagination").isVisible(), count > 0);
      if (count) await assertPage(1, Math.ceil(count / 24), count, Math.min(count, 24));
    }

    await seed(73);
    await page.locator("#page-next").click();
    await assertPage(2, 4, 73, 24);
    assert.equal(await page.locator(".artwork-pick span").first().innerText(), "Artwork 25");
    await page.locator("#page-previous").click();
    await assertPage(1, 4, 73, 24);
    await page.locator("#page-last").click();
    await assertPage(4, 4, 73, 1);
    await page.locator("#page-first").click();
    await page.locator("#page-numbers").getByRole("button", { name: "Page 3", exact: true }).click();
    await assertPage(3, 4, 73, 24);
    assert.equal(await page.evaluate(() => document.activeElement.getAttribute("aria-current")), "page");

    await jump(2, true);
    await assertPage(2, 4, 73, 24);
    await jump("0003");
    await assertPage(3, 4, 73, 24);

    for (const invalid of ["", "0", "-1", "5", "1.5", "1e2", "99999999999999999999"]) {
      await jump(invalid);
      assert.equal(await page.locator("#page-jump").getAttribute("aria-invalid"), "true");
      assert.match(await page.locator("#page-jump").evaluate((input) => input.validationMessage), /whole page number from 1 to 4/);
      assert.match(await page.locator("#pagination-summary").innerText(), /^Page 3 of 4/);
    }
    await jump(1);
    await assertPage(1, 4, 73, 24);

    await seed(2401);
    await jump(51);
    await assertPage(51, 101, 2401, 24);
    assert.equal(await page.locator(".pagination-ellipsis").count(), 2);
    assert((await page.locator("#page-numbers button").count()) <= 7);
    await page.locator("#page-last").click();
    await assertPage(101, 101, 2401, 1);
    await page.locator(".artwork-pick").click();
    await page.locator("#page-first").click();
    assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 2401");

    // Real add/export path: exceed the former 30-item cap and cross a page boundary.
    const png = await page.locator("#collection-preview").evaluate((image) => {
      const canvas = document.createElement("canvas");
      canvas.width = 60;
      canvas.height = 40;
      canvas.getContext("2d").fillRect(0, 0, 60, 40);
      return canvas.toDataURL().split(",")[1];
    });
    await page.locator("#file-input").setInputFiles({
      name: "New artwork.png",
      mimeType: "image/png",
      buffer: Buffer.from(png, "base64"),
    });
    await page.waitForFunction(() => !document.querySelector("#add-to-gallery").disabled);
    for (const before of [30, 48]) {
      await seed(before);
      await page.locator("#add-to-gallery").click();
      await page.waitForFunction((count) => collection.length === count, before + 1);
      await assertPage(Math.ceil((before + 1) / 24), Math.ceil((before + 1) / 24), before + 1, (before % 24) + 1);
      assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 1");
    }

    // Cancel preserves the collection; deleting the sole item on page 3 clamps to 2.
    await page.locator(".delete-artwork").click();
    await page.locator("#cancel-delete").click();
    await assertPage(3, 3, 49, 1);
    await page.locator(".delete-artwork").click();
    await page.locator("#confirm-delete").click();
    await page.waitForFunction(() => collection.length === 48);
    await assertPage(2, 2, 48, 24);
    assert.equal(await page.locator("#page-jump").getAttribute("aria-invalid"), null);

    await seed(49);
    await page.locator(".artwork-pick").last().click();
    await page.clock.install();
    await page.locator("#slideshow-interval").selectOption("180");
    await page.locator("#slideshow-toggle").click();
    await page.clock.fastForward(179000);
    assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 24");
    await page.clock.fastForward(2000);
    assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 25");
    await assertPage(1, 3, 49, 24);
    await jump(2);
    assert.equal(await page.locator(".collection-item.selected .artwork-pick span").innerText(), "Artwork 25");
    assert.equal(await page.locator("#slideshow-toggle").getAttribute("aria-pressed"), "true");
    await page.clock.fastForward(180000);
    assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 26");
    await page.locator("#page-last").click();
    await page.locator(".artwork-pick").click();
    await page.clock.fastForward(180000);
    assert.equal(await page.locator("#collection-current-name").innerText(), "Artwork 1");
    await assertPage(3, 3, 49, 1);
    await page.locator("#slideshow-toggle").click();

    await seed(1);
    await page.locator(".delete-artwork").click();
    await page.locator("#confirm-delete").click();
    await page.waitForFunction(() => collection.length === 0);
    assert.equal(await page.locator("#collection-pagination").isVisible(), false);
    assert.equal(await page.locator("#collection-empty").isVisible(), true);

    await seed(2401);
    for (const current of [1, 4, 51, 98, 101]) {
      await jump(current);
      const pages = await page.locator("#page-numbers button").evaluateAll((buttons) => buttons.map((button) => Number(button.dataset.page)));
      assert(pages.includes(1) && pages.includes(101) && pages.includes(current));
      assert.deepEqual(pages, [...new Set(pages)].sort((a, b) => a - b));
      assert(pages.length <= 7);
      for (const width of [320, 375, 768, 1440]) {
        await page.setViewportSize({ width, height: 900 });
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, `Overflow at ${width}px, page ${current}`);
      }
    }
    await jump(51);
    if (process.env.INKHIBIT_SCREENSHOT_DIR) {
      mkdirSync(process.env.INKHIBIT_SCREENSHOT_DIR, { recursive: true });
      for (const width of [375, 1440]) {
        await page.setViewportSize({ width, height: 900 });
        await page.locator("#collection-pagination").screenshot({ path: path.join(process.env.INKHIBIT_SCREENSHOT_DIR, `pagination-${width}.png`) });
      }
    }

    await page.locator("#logout").click();
    assert.equal(await page.locator("main").isVisible(), false);
    await page.locator("#login-password").fill("demo-again");
    await page.getByRole("button", { name: "Enter your gallery" }).click();
    await assertPage(51, 101, 2401, 24);
    await page.reload();
    assert.equal(await page.locator("#login-screen").isVisible(), true);
    assert.deepEqual(errors, []);
    assert(methods.every((method) => method === "GET"));
    console.log("PASS: pagination boundaries, shortcuts, jumps, invalid input, adding, deletion, full-collection slideshow, mobile layouts, logout, and reload.");
  } finally {
    if (browser) await browser.close();
    await new Promise((resolve) => server.close(resolve));
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
