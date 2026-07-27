"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

function manifestRow(id, datetime, display) {
  return {
    querySelectorAll: function (selector) {
      if (selector === "a") return [{ textContent: id }];
      if (selector === "td") return [{ innerText: display }];
      return [];
    },
    querySelector: function (selector) {
      return selector === "[datetime], [data-time]" ? {
        getAttribute: function (name) { return name === "data-time" ? datetime : null; }
      } : null;
    }
  };
}

const rows = [
  manifestRow("5753583882400741046", "2026-07-07T22:11:00+00:00",
    "7 July 2026 – 22:11:00 UTC"),
  manifestRow("3514306556860204959", "2025-07-14T23:27:11+00:00",
    "14 July 2025 – 23:27:11 UTC")
];
const table = {
  tagName: "TABLE",
  querySelectorAll: function (selector) {
    if (selector === "tbody tr") return rows;
    if (selector === "th") return [
      { textContent: "Seen Date" }, { textContent: "Relative Date" },
      { textContent: "ManifestID" }
    ];
    return [];
  }
};
const pane = {
  querySelectorAll: function (selector) { return selector === "table" ? [table] : []; }
};
const heading = {
  textContent: "Previously seen manifests",
  nextElementSibling: { tagName: "DIV", nextElementSibling: { tagName: "DIV" } },
  parentElement: pane
};
let listener;
const context = {
  location: { pathname: "/depot/228989/manifests/" },
  setTimeout: setTimeout,
  chrome: { runtime: { onMessage: { addListener: function (value) { listener = value; } } } },
  document: {
    querySelectorAll: function (selector) {
      return selector === "h1,h2,h3" ? [heading] : [];
    }
  }
};

vm.runInNewContext(fs.readFileSync(
  path.join(__dirname, "steamdb-history-extension/content.js"), "utf8"
), context);
new Promise(function (resolve, reject) {
  listener({ action: "depot_manifests" }, {}, function (reply) {
    if (!reply.ok) return reject(new Error(reply.error));
    resolve(reply.value);
  });
}).then(function (value) {
  assert.deepStrictEqual(JSON.parse(JSON.stringify(value)), {
    depot_id: "228989",
    manifests: [
      {
        manifest_id: "5753583882400741046",
        seen_at: "2026-07-07T22:11:00.000Z",
        seen_display: "7 July 2026 – 22:11:00 UTC"
      },
      {
        manifest_id: "3514306556860204959",
        seen_at: "2025-07-14T23:27:11.000Z",
        seen_display: "14 July 2025 – 23:27:11 UTC"
      }
    ]
  });
  console.log("test_steamdb_depot_manifests_extension: ALL PASS");
}).catch(function (error) {
  console.error(error.stack || error);
  process.exitCode = 1;
});
