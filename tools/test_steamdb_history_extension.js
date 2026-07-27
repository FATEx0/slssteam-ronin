"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

function cell(text, bytes) {
  return {
    innerText: text,
    getAttribute: function (name) { return name === "data-sort" ? bytes : null; }
  };
}

const configuration = {
  innerText: "DLC 4689760inKONBINI Artbook",
  children: [
    { innerText: "DLC 4689760" },
    { innerText: "inKONBINI Artbook" }
  ],
  querySelector: function (selector) {
    return selector === ".i.muted" ? { innerText: "inKONBINI Artbook" } : null;
  }
};
const row = {
  getAttribute: function (name) { return name === "data-depotid" ? "4689760" : null; },
  querySelector: function (selector) {
    if (selector === "a.app[data-appid]") {
      return {
        innerText: "DLC 4689760",
        getAttribute: function () { return "4689760"; }
      };
    }
    if (selector === ".depot-config") return configuration;
    if (selector === ".depot-size-disk") return cell("6.13 MiB", "6428706");
    if (selector === ".depot-size-download") return cell("5.90 MiB", "6186598");
    return null;
  }
};
const table = {
  tagName: "TABLE",
  querySelectorAll: function (selector) {
    return selector === "tbody tr.depot[data-depotid]" ? [row] : [];
  }
};
const heading = { tagName: "H2", textContent: "Depots", nextElementSibling: table };
const redistributableConfiguration = {
  innerText: "WindowsShared InstallDepot from 228980VC 2022 Redist",
  children: [
    { innerText: "Windows" }, { innerText: "Shared Install" },
    { innerText: "Depot from 228980" }, { innerText: "VC 2022 Redist" }
  ],
  querySelector: function (selector) {
    return selector === ".i.muted" ? { innerText: "VC 2022 Redist" } : null;
  }
};
const redistributableRow = {
  getAttribute: function (name) { return name === "data-depotid" ? "228989" : null; },
  querySelector: function (selector) {
    if (selector === "a.app[data-appid]") {
      return {
        innerText: "Depot from 228980",
        getAttribute: function () { return "228980"; }
      };
    }
    if (selector === ".depot-config") return redistributableConfiguration;
    if (selector === ".depot-size-disk") return cell("No size", "0");
    if (selector === ".depot-size-download") return cell("", "0");
    return null;
  }
};
const redistributableTable = {
  tagName: "TABLE",
  querySelectorAll: function (selector) {
    return selector === "tbody tr.depot[data-depotid]" ? [redistributableRow] : [];
  }
};
const redistributableHeading = {
  tagName: "H2", textContent: "Redistributables",
  nextElementSibling: redistributableTable
};
const innerDlcHeading = {
  tagName: "H2", textContent: "Inner depots from DLC", nextElementSibling: table
};
const pane = { children: [heading, innerDlcHeading, redistributableHeading] };
let listener;
const context = {
  location: { pathname: "/app/2723430/depots/" },
  setTimeout: setTimeout,
  clearTimeout: clearTimeout,
  chrome: { runtime: { onMessage: { addListener: function (value) { listener = value; } } } },
  document: {
    querySelector: function (selector) {
      if (selector === ".scope-app[data-appid]") {
        return { getAttribute: function () { return "2723430"; } };
      }
      if (selector === "#depots") return pane;
      if (selector === "#branches") return null;
      return null;
    }
  }
};

vm.runInNewContext(fs.readFileSync(
  path.join(__dirname, "steamdb-history-extension/content.js"), "utf8"
), context);
assert(listener, "content script did not register its message listener");

new Promise(function (resolve, reject) {
  listener({ action: "depots" }, {}, function (reply) {
    if (!reply.ok) return reject(new Error(reply.error));
    resolve(reply.value);
  });
}).then(function (value) {
  assert.deepStrictEqual(JSON.parse(JSON.stringify(value.depots[0])), {
    depot_id: "4689760",
    owner_appid: "4689760",
    category: "dlc",
    section: "Depots",
    descriptor: "inKONBINI Artbook",
    configuration: "DLC 4689760 · inKONBINI Artbook",
    size: { bytes: "6428706", display: "6.13 MiB" },
    dl: { bytes: "6186598", display: "5.90 MiB" },
    appearances: [
      {
        category: "dlc", section: "Depots", descriptor: "inKONBINI Artbook",
        configuration: "DLC 4689760 · inKONBINI Artbook",
        size: { bytes: "6428706", display: "6.13 MiB" },
        dl: { bytes: "6186598", display: "5.90 MiB" }
      },
      {
        category: "inner-depots-from-dlc", section: "Inner depots from DLC",
        descriptor: "inKONBINI Artbook",
        configuration: "DLC 4689760 · inKONBINI Artbook",
        size: { bytes: "6428706", display: "6.13 MiB" },
        dl: { bytes: "6186598", display: "5.90 MiB" }
      }
    ]
  });
  assert.deepStrictEqual(JSON.parse(JSON.stringify(value.depots[1])), {
    depot_id: "228989",
    owner_appid: "228980",
    category: "redistributable",
    section: "Redistributables",
    descriptor: "VC 2022 Redist",
    configuration: "Windows · Shared Install · Depot from 228980 · VC 2022 Redist",
    size: { bytes: "0", display: "No size" },
    dl: { bytes: "0", display: null },
    appearances: [
      {
        category: "redistributable", section: "Redistributables",
        descriptor: "VC 2022 Redist",
        configuration: "Windows · Shared Install · Depot from 228980 · VC 2022 Redist",
        size: { bytes: "0", display: "No size" },
        dl: { bytes: "0", display: null }
      }
    ]
  });
  assert.strictEqual(value.depots.length, 2, "duplicate DLC depot was not canonicalized");
  console.log("test_steamdb_history_extension: ALL PASS");
}).catch(function (error) {
  console.error(error.stack || error);
  process.exitCode = 1;
});
