var root;
var currentContext;
var currentState;

function element(tag, className, text) {
  var node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function asArray(value) {
  return Array.isArray(value) ? value : [];
}

function rootBuilds(history) {
  if (!history || typeof history !== "object") return [];
  if (Array.isArray(history.apps)) {
    var rootHistory = history.apps.find(function (app) {
      return String(app.appid) === String(history.appid);
    });
    return rootHistory && Array.isArray(rootHistory.builds) ? rootHistory.builds : [];
  }
  return asArray(history.builds);
}

function sameDepots(left, right) {
  left = left || {};
  right = right || {};
  var ids = Object.keys(left);
  if (ids.length !== Object.keys(right).length) return false;
  return ids.every(function (id) { return String(left[id]) === String(right[id]); });
}

function formatBuild(build) {
  var label = "Build " + String(build.build_id);
  if (build.published_at) {
    var date = new Date(build.published_at);
    if (!isNaN(date.getTime())) label += " \u00b7 " + date.toLocaleDateString();
  }
  return label;
}

function importHistory(context, game, observation) {
  if (String(observation.appid) !== String(game.appid)) {
    throw new Error("History file is for app " + observation.appid +
      ", not app " + game.appid + ".");
  }
  if (!game.installed_build_id || !Object.keys(game.manifests || {}).length) {
    throw new Error("This app has no complete local anchor snapshot.");
  }
  var payload = {
    appid: String(observation.appid),
    anchor: {
      build_id: String(game.installed_build_id),
      depots: game.manifests
    }
  };
  if (Array.isArray(observation.apps)) payload.apps = observation.apps;
  else payload.builds = observation.builds;
  if (Array.isArray(observation.shared_depot_histories)) {
    payload.shared_depot_histories = observation.shared_depot_histories;
  }
  if (observation.availability_policy &&
      typeof observation.availability_policy === "object") {
    payload.availability_policy = observation.availability_policy;
  }
  return context.call("pins.history.import", payload);
}

function validateApp(context, appid) {
  return context.call("tsuki.steam.app.validate", { appid: String(appid) });
}

function applyBuild(context, game, build, status, button) {
  button.disabled = true;
  status.className = "message working";
  status.textContent = "Resolving build " + build.build_id + "\u2026";
  context.call("pins.history.resolve", {
    appid: String(game.appid),
    build_id: String(build.build_id)
  }).then(function (resolved) {
    status.textContent = "Saving build lock\u2026";
    return context.call("pins.set", {
      appid: String(game.appid),
      build_id: String(build.build_id),
      locked: true,
      depots: resolved.depots || {}
    });
  }).then(function () {
    status.textContent = "Starting Steam validation\u2026";
    return new Promise(function (resolve) { setTimeout(resolve, 1000); });
  }).then(function () {
    return validateApp(context, game.appid);
  }).then(function () {
    status.className = "message success";
    status.textContent = "Build " + build.build_id +
      " saved. Steam is validating and will download the required files.";
    return context.call("pins.list", {});
  }).then(function (state) {
    currentState = state;
    setTimeout(function () { render(context, state); }, 900);
  }).catch(function (error) {
    status.className = "message";
    status.textContent = String(error && error.message || error);
    button.disabled = false;
  });
}

function applyLatest(context, game, status, button) {
  button.disabled = true;
  status.className = "message working";
  status.textContent = "Removing build lock\u2026";
  context.call("pins.clear", { appid: String(game.appid) }).then(function () {
    status.textContent = "Starting Steam validation\u2026";
    return new Promise(function (resolve) { setTimeout(resolve, 1000); });
  }).then(function () {
    return validateApp(context, game.appid);
  }).then(function () {
    status.className = "message success";
    status.textContent = "Live build selected. Steam is validating and will update the files.";
    return context.call("pins.list", {});
  }).then(function (state) {
    currentState = state;
    setTimeout(function () { render(context, state); }, 900);
  }).catch(function (error) {
    status.className = "message";
    status.textContent = String(error && error.message || error);
    button.disabled = false;
  });
}

function buildRow(label, detail, badges, selected, onClick) {
  var row = element("button", "build-row" + (selected ? " selected" : ""));
  row.type = "button";
  var radio = element("span", "build-radio");
  var text = element("span", "build-text");
  text.appendChild(element("strong", "", label));
  if (detail) text.appendChild(element("small", "", detail));
  var badgeWrap = element("span", "badges");
  (badges || []).forEach(function (badge) {
    badgeWrap.appendChild(element("span", "badge " + badge.kind, badge.text));
  });
  row.appendChild(radio);
  row.appendChild(text);
  row.appendChild(badgeWrap);
  row.addEventListener("click", onClick);
  return row;
}

function renderTimeline(context, game, pin, container, status) {
  container.textContent = "Loading build history\u2026";
  context.call("pins.history.list", { appid: String(game.appid) }).then(function (history) {
    var builds = rootBuilds(history);
    container.textContent = "";
    var rows = [];
    var select = function (row) {
      rows.forEach(function (item) { item.classList.toggle("selected", item === row); });
    };
    var latest;
    latest = buildRow(
      "Live build",
      "Follow Steam\u2019s current public build",
      pin ? [] : [{ kind: "active", text: "Selected" }],
      !pin,
      function () {
        select(latest);
        applyLatest(context, game, status, latest);
      }
    );
    rows.push(latest);
    container.appendChild(latest);

    var installedMatch = builds.find(function (build) {
      return sameDepots(build.depots, game.manifests);
    });
    var hidden = [];
    builds.forEach(function (build, index) {
      var selected = !!pin && String(pin.build_id) === String(build.build_id);
      var badges = [];
      if (selected) badges.push({ kind: "locked", text: "Locked" });
      if (installedMatch && String(installedMatch.build_id) === String(build.build_id)) {
        badges.push({ kind: "current", text: "Installed" });
      }
      var row;
      row = buildRow(
        formatBuild(build),
        String(build.build_id),
        badges,
        selected,
        function () {
          select(row);
          applyBuild(context, game, build, status, row);
        }
      );
      rows.push(row);
      container.appendChild(row);
      if (index >= 3 && !selected) {
        row.hidden = true;
        hidden.push(row);
      }
    });
    if (hidden.length) {
      var more = element("button", "show-more", "Show " + hidden.length + " older builds");
      more.type = "button";
      var open = false;
      more.addEventListener("click", function () {
        open = !open;
        hidden.forEach(function (row) { row.hidden = !open; });
        more.textContent = open ? "Show fewer builds" : "Show " + hidden.length + " older builds";
      });
      container.appendChild(more);
    }
    if (!builds.length) {
      container.appendChild(element("div", "empty compact",
        "No cached build history. Import a SteamDB history JSON file above."));
    }
  }).catch(function (error) {
    container.textContent = "";
    status.textContent = String(error && error.message || error);
  });
}

function gameCard(context, game, pin) {
  var card = element("section", "pin-card");
  var title = element("div", "pin-title");
  var summary = element("div", "game-summary");
  var artwork = element("img", "game-art");
  artwork.alt = "";
  artwork.draggable = false;
  artwork.loading = "lazy";
  artwork.src = "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/"
    + encodeURIComponent(game.appid) + "/header.jpg";
  artwork.addEventListener("error", function () { artwork.hidden = true; });
  var identity = element("div");
  identity.appendChild(element("strong", "", game.title || ("App " + game.appid)));
  identity.appendChild(element("small", "", "App " + game.appid));
  identity.appendChild(element("small", "build",
    pin ? "Locked to build " + pin.build_id :
      "Steam build " + (game.installed_build_id || "unknown")));
  summary.appendChild(artwork);
  summary.appendChild(identity);
  title.appendChild(summary);
  title.appendChild(element("span", pin ? "state locked-state" : "state",
    pin ? "Locked" : "Live"));
  card.appendChild(title);
  var status = element("div", "message card-message");
  var timeline = element("div", "build-list");
  card.appendChild(timeline);
  card.appendChild(status);
  renderTimeline(context, game, pin, timeline, status);
  return card;
}

function render(context, state) {
  currentContext = context;
  currentState = state || {};
  var configuredApps = asArray(currentState.apps);
  var managedGames = asArray(currentState.games);
  var pins = {};
  configuredApps.forEach(function (pin) { pins[String(pin.appid)] = pin; });
  configuredApps.forEach(function (pin) {
    if (!managedGames.some(function (game) {
      return String(game.appid) === String(pin.appid);
    })) {
      managedGames.push({
        appid: String(pin.appid),
        title: "App " + pin.appid,
        installed_build_id: pin.build_id,
        manifests: pin.depots || {}
      });
    }
  });
  managedGames.sort(function (a, b) {
    return String(a.title || a.appid).localeCompare(String(b.title || b.appid));
  });

  root.textContent = "";
  var intro = element("div", "intro");
  var introText = element("div");
  introText.appendChild(element("h1", "", "Game Builds"));
  introText.appendChild(element("p", "",
    "Choose one complete build for a managed game. Ronin resolves and locks every depot belonging to that build together."));
  intro.appendChild(introText);

  var importButton = element("button", "import-button", "\u2191 Import history JSON");
  importButton.type = "button";
  var fileInput = element("input");
  fileInput.type = "file";
  fileInput.accept = "application/json,.json";
  fileInput.hidden = true;
  var importStatus = element("div", "message import-message");
  importButton.addEventListener("click", function () {
    fileInput.value = "";
    fileInput.click();
  });
  fileInput.addEventListener("change", function () {
    var file = fileInput.files && fileInput.files[0];
    if (!file) return;
    importButton.disabled = true;
    importStatus.className = "message import-message working";
    importStatus.textContent = "Importing " + file.name + "\u2026";
    file.text().then(function (text) {
      var observation = JSON.parse(text);
      var game = managedGames.find(function (item) {
        return String(item.appid) === String(observation.appid);
      });
      if (!game) throw new Error("App " + observation.appid + " is not a managed game.");
      return importHistory(context, game, observation);
    }).then(function () {
      importStatus.className = "message import-message success";
      importStatus.textContent = "History imported successfully.";
      return context.call("pins.list", {});
    }).then(function (next) {
      setTimeout(function () { render(context, next); }, 700);
    }).catch(function (error) {
      importStatus.className = "message import-message";
      importStatus.textContent = String(error && error.message || error);
      importButton.disabled = false;
    });
  });
  var importWrap = element("div", "import-wrap");
  importWrap.appendChild(importButton);
  importWrap.appendChild(fileInput);
  intro.appendChild(importWrap);
  root.appendChild(intro);
  root.appendChild(importStatus);

  var list = element("div", "pin-list");
  managedGames.forEach(function (game) {
    list.appendChild(gameCard(context, game, pins[String(game.appid)]));
  });
  if (!managedGames.length) {
    list.appendChild(element("div", "empty", "No managed games discovered."));
  }
  root.appendChild(list);
}

function mount(context) {
  root = context.container;
  root.className = "manifest-pins-page";
  root.textContent = "Loading game builds\u2026";
  context.call("pins.list", {}).then(function (state) {
    render(context, state);
  }).catch(function (error) {
    root.textContent = "Unable to load game builds: " + String(error && error.message || error);
  });
}

function unmount() {
  if (root) root.textContent = "";
  root = null;
  currentContext = null;
  currentState = null;
}
