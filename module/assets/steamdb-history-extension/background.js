function message(tabId, payload) {
  return new Promise(function (resolve, reject) {
    var attempts = 0;
    function send() {
      chrome.tabs.sendMessage(tabId, payload, function (reply) {
        var error = chrome.runtime.lastError;
        if (error && /Receiving end does not exist|Could not establish connection/i.test(
          error.message || "") && attempts++ < 50) {
          setTimeout(send, 100);
          return;
        }
        if (error) return reject(new Error(error.message));
        if (!reply || !reply.ok)
          return reject(new Error(reply && reply.error || "Collector did not reply"));
        resolve(reply.value);
      });
    }
    send();
  });
}

function open(url) {
  return new Promise(function (resolve) {
    // SteamDB lazily loads depot history. Firefox may throttle that work in a
    // never-visible background tab, so each short-lived collection tab is
    // deliberately activated.
    chrome.tabs.create({ url: url, active: true }, resolve);
  });
}

function navigate(tabId, url) {
  return new Promise(function (resolve, reject) {
    var began = false;
    function listener(changedId, info) {
      if (changedId !== tabId) return;
      if (info.status === "loading" || info.url) began = true;
      if (!began || info.status !== "complete") return;
      chrome.tabs.onUpdated.removeListener(listener);
      chrome.tabs.get(tabId, resolve);
    }
    chrome.tabs.onUpdated.addListener(listener);
    chrome.tabs.update(tabId, { url: url, active: true }, function () {
      if (!chrome.runtime.lastError) return;
      chrome.tabs.onUpdated.removeListener(listener);
      reject(new Error(chrome.runtime.lastError.message));
    });
  });
}

function tabInfo(tabId) {
  return new Promise(function (resolve) { chrome.tabs.get(tabId, resolve); });
}

function loaded(tabId) {
  return new Promise(function (resolve) {
    chrome.tabs.get(tabId, function (tab) {
      if (tab && tab.status === "complete") {
        resolve();
      } else {
        function listener(changedId, info) {
          if (changedId === tabId && info.status === "complete") {
            chrome.tabs.onUpdated.removeListener(listener);
            resolve();
          }
        }
        chrome.tabs.onUpdated.addListener(listener);
      }
    });
  });
}

function close(tabId) {
  return new Promise(function (resolve) { chrome.tabs.remove(tabId, resolve); });
}

function downloadJson(payload, filename) {
  return new Promise(function (resolve, reject) {
    var objectUrl = null;
    var url;
    if (typeof URL.createObjectURL === "function") {
      objectUrl = URL.createObjectURL(new Blob([payload], { type: "application/json" }));
      url = objectUrl;
    } else {
      // Chromium MV3 service workers do not expose createObjectURL.
      url = "data:application/json;charset=utf-8," + encodeURIComponent(payload);
    }
    chrome.downloads.download({ url: url, filename: filename, saveAs: true },
      function (downloadId) {
        var error = chrome.runtime.lastError;
        if (objectUrl) setTimeout(function () { URL.revokeObjectURL(objectUrl); }, 30000);
        if (error) reject(new Error(error.message));
        else resolve(downloadId);
      });
  });
}

chrome.runtime.onMessage.addListener(function (request, sender, respond) {
  if (request.action !== "collect") return;
  (async function () {
    var tabs = await new Promise(function (resolve) {
      chrome.tabs.query({ active: true, currentWindow: true }, resolve);
    });
    if (!tabs[0]) throw new Error("No active tab");
    var rootList = await message(tabs[0].id, { action: "list" });
    var sourceTab = tabs[0].id;
    await message(sourceTab, {
      action: "status",
      message: "Ronin collector: found " + rootList.builds.length + " base builds."
    });
    var collectorTab = null;
    var succeeded = false;
    var appObservations = [];
    var sharedDepotHistories = [];
    try {
      collectorTab = await open("https://steamdb.info/app/" + rootList.appid + "/dlc/");
      await loaded(collectorTab.id);
      var relations = await message(collectorTab.id, { action: "dlcs" });
      var appids = [rootList.appid].concat(relations.dlc_appids);
      for (var appIndex = 0; appIndex < appids.length; ++appIndex) {
        var appid = appids[appIndex];
        await navigate(collectorTab.id,
          "https://steamdb.info/app/" + appid + "/depots/");
        var finalTab = await tabInfo(collectorTab.id);
        var expectedDepotPath = "/app/" + appid + "/depots";
        var depotInfo;
        var finalPath = finalTab &&
          new URL(finalTab.url).pathname.replace(/\/+$/, "");
        if (finalPath !== expectedDepotPath) {
          depotInfo = {
            appid: appid, downloadable: false,
            depots: [], branches: []
          };
        } else {
          depotInfo = await message(collectorTab.id, { action: "depots" });
        }
        if (!depotInfo.downloadable) {
          appObservations.push({
            appid: appid,
            relation: appid === rootList.appid ? "root" : "dlc",
            downloadable: false,
            depots: [],
            branches: [],
            builds: []
          });
          continue;
        }
        var list = rootList;
        if (appid !== rootList.appid) {
          await navigate(collectorTab.id,
            "https://steamdb.info/app/" + appid + "/patchnotes/");
          list = await message(collectorTab.id, { action: "list" });
        }
        var observations = [];
        for (var index = 0; index < list.builds.length; ++index) {
          var build = list.builds[index];
          await message(sourceTab, {
            action: "status",
            message: "Ronin collector: app " + appid + ", build " + build.build_id +
              " (" + (index + 1) + "/" + list.builds.length + ")…"
          });
          await navigate(collectorTab.id,
            "https://steamdb.info/patchnotes/" + build.build_id + "/");
          var observation = await message(collectorTab.id, {
            action: "patch",
            terminal: index + 1 === list.builds.length
          });
          observation.published_at = build.published_at;
          observations.push(observation);
        }
        appObservations.push({
          appid: appid,
          relation: appid === rootList.appid ? "root" : "dlc",
          downloadable: true,
          depots: depotInfo.depots,
          branches: depotInfo.branches,
          builds: observations
        });
      }
      var sharedDepots = appObservations[0].depots.filter(function (depot) {
        return depot.category === "shared" || depot.category === "redistributable";
      });
      for (var sharedIndex = 0; sharedIndex < sharedDepots.length; ++sharedIndex) {
        var sharedDepot = sharedDepots[sharedIndex];
        await message(sourceTab, {
          action: "status",
          message: "Ronin collector: shared depot " + sharedDepot.depot_id +
            " manifests (" + (sharedIndex + 1) + "/" + sharedDepots.length + ")…"
        });
        await navigate(collectorTab.id,
          "https://steamdb.info/depot/" + sharedDepot.depot_id + "/manifests/");
        var sharedHistory = await message(collectorTab.id, { action: "depot_manifests" });
        sharedHistory.owner_appid = sharedDepot.owner_appid;
        sharedHistory.category = sharedDepot.category;
        sharedDepotHistories.push(sharedHistory);
      }
      succeeded = true;
    } finally {
      if (succeeded && collectorTab) await close(collectorTab.id);
      if (succeeded) chrome.tabs.update(sourceTab, { active: true });
    }
    var payload = JSON.stringify({
      schema_version: "1.0",
      source: "steamdb-browser-observation",
      appid: rootList.appid,
      apps: appObservations,
      shared_depot_histories: sharedDepotHistories,
      availability_policy: {
        dlc_release_between_base_builds: "preceding_base_build"
      }
    }, null, 2);
    await downloadJson(payload, "ronin-steamdb-" + rootList.appid + ".json");
    await message(sourceTab, {
      action: "status",
      message: "Ronin collector: exported " + appObservations.length + " app histories."
    });
    return { count: appObservations.length };
  }()).then(function (value) { respond({ ok: true, value: value }); },
    function (error) {
      chrome.tabs.query({ active: true, currentWindow: true }, function (tabs) {
        if (tabs[0]) message(tabs[0].id, {
          action: "status",
          message: "Ronin collector failed:\n" + String(error.message || error)
        }).catch(function () {});
      });
      respond({ ok: false, error: String(error.message || error) });
    });
  return true;
});
