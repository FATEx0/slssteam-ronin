var root;
var currentContext;
var currentState;

function element(tag, className, text) {
  var node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function importIcon() {
  var icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  icon.setAttribute("viewBox", "0 0 24 24");
  icon.setAttribute("width", "20");
  icon.setAttribute("height", "20");
  icon.setAttribute("fill", "none");
  icon.setAttribute("stroke", "currentColor");
  icon.setAttribute("stroke-width", "2");
  icon.setAttribute("stroke-linecap", "round");
  icon.setAttribute("stroke-linejoin", "round");
  icon.innerHTML = "<ellipse cx=\"9\" cy=\"5\" rx=\"6\" ry=\"3\"/>" +
    "<path d=\"M3 5v6c0 1.7 2.7 3 6 3 1.1 0 2.1-.1 3-.4\"/>" +
    "<path d=\"M3 11v6c0 1.7 2.7 3 6 3 2.3 0 4.3-.6 5.3-1.5\"/>" +
    "<path d=\"M18 5v10m0 0-3-3m3 3 3-3\"/>";
  return icon;
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

function importHistory(context, game, observation, rawText, file, gestureToken) {
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
  var bytes = new TextEncoder().encode(rawText);
  return crypto.subtle.digest("SHA-256", bytes).then(function (digest) {
    var hex = Array.from(new Uint8Array(digest)).map(function (value) {
      return value.toString(16).padStart(2, "0");
    }).join("");
    return context.importData("steamdb.history.observation", {
      raw: rawText,
      payload: payload,
      media_type: (file && file.type) || "application/json",
      source: {
        name: (file && file.name) || "selected JSON file",
        size: bytes.byteLength,
        last_modified: file && file.lastModified || 0
      },
      sha256: hex,
      gesture_token: gestureToken
    });
  });
}

function waitOperation(context, reference) {
  return context.call("tsuki.operation.get", {
    operation_id: String(reference.operation_id)
  }).then(function (state) {
    if (state.state === "succeeded") return state.result || {};
    if (state.state === "failed" || state.state === "cancelled") {
      throw new Error(state.error && (state.error.detail || state.error.code) ||
        "Manifest operation failed.");
    }
    return new Promise(function (resolve) { setTimeout(resolve, 200); })
      .then(function () { return waitOperation(context, reference); });
  });
}

function applyBuild(context, game, build, status, button) {
  var gesture;
  try { gesture = context.beginOperation("manifest-pack.install"); }
  catch (error) {
    status.textContent = String(error && error.message || error);
    return;
  }
  button.disabled = true;
  status.className = "message working";
  status.textContent = "Inspecting build " + build.build_id + "\u2026";
  context.call("manifest-pack.inspect", {
    app_id: String(game.appid),
    build_id: String(build.build_id),
    action: "install"
  }).then(function (inspected) {
    status.textContent = "Confirming and applying manifest pack\u2026";
    return context.operation("manifest-pack.install", {
      app_id: String(game.appid),
      plan: inspected.plan,
      plan_digest: inspected.plan_digest
    }, gesture);
  }).then(function (reference) {
    return waitOperation(context, reference);
  }).then(function () {
    status.className = "message success";
    status.textContent = "Build " + build.build_id +
      " confirmed. Steam is validating and will download the required files.";
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
  var gesture;
  try { gesture = context.beginOperation("manifest-pack.remove"); }
  catch (error) {
    status.textContent = String(error && error.message || error);
    return;
  }
  button.disabled = true;
  status.className = "message working";
  status.textContent = "Inspecting current manifest pack\u2026";
  context.call("manifest-pack.inspect", {
    app_id: String(game.appid),
    action: "remove"
  }).then(function (inspected) {
    status.textContent = "Confirming Live build\u2026";
    return context.operation("manifest-pack.remove", {
      app_id: String(game.appid),
      plan: inspected.plan,
      plan_digest: inspected.plan_digest
    }, gesture);
  }).then(function (reference) {
    return waitOperation(context, reference);
  }).then(function () {
    status.className = "message success";
    status.textContent = "Live build confirmed. Steam is validating and will update the files.";
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
    });

    var inactive = rows.filter(function (row) {
      return !row.classList.contains("selected");
    });
    if (inactive.length) {
      inactive.forEach(function (row) { row.hidden = true; });
      var more = element("button", "show-more", "Show all builds");
      more.type = "button";
      var open = false;
      more.addEventListener("click", function () {
        open = !open;
        inactive.forEach(function (row) { row.hidden = !open; });
        more.textContent = open ? "Show active build only" : "Show all builds";
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
  var toolbar = element("div", "page-toolbar");
  var search = element("input", "game-search");
  search.type = "search";
  search.placeholder = "Search games";
  search.autocomplete = "off";

  var importButton = element("button", "import-button");
  importButton.type = "button";
  importButton.title = "Import history JSON";
  importButton.appendChild(importIcon());
  var fileInput = element("input");
  fileInput.type = "file";
  fileInput.accept = "application/json,.json";
  fileInput.hidden = true;
  var importStatus = element("div", "message import-message");
  var pendingGestureToken = null;
  importButton.addEventListener("click", function () {
    try {
      pendingGestureToken = context.beginDataImport("steamdb.history.observation");
    } catch (error) {
      importStatus.textContent = String(error && error.message || error);
      return;
    }
    fileInput.value = "";
    fileInput.click();
  });
  fileInput.addEventListener("change", function () {
    var file = fileInput.files && fileInput.files[0];
    if (!file) return;
    var gestureToken = pendingGestureToken;
    pendingGestureToken = null;
    importButton.disabled = true;
    importStatus.className = "message import-message working";
    importStatus.textContent = "Importing " + file.name + "\u2026";
    file.text().then(function (text) {
      var observation = JSON.parse(text);
      var game = managedGames.find(function (item) {
        return String(item.appid) === String(observation.appid);
      });
      if (!game) throw new Error("App " + observation.appid + " is not a managed game.");
      return importHistory(context, game, observation, text, file, gestureToken);
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
  toolbar.appendChild(search);
  toolbar.appendChild(importWrap);
  root.appendChild(toolbar);
  root.appendChild(importStatus);

  var pinnedSection = element("section", "game-section");
  var pinnedHeading = element("button", "section-heading");
  pinnedHeading.type = "button";
  pinnedHeading.appendChild(element("span", "section-chevron", "\u25be"));
  pinnedHeading.appendChild(element("span", "section-label", "Pinned"));
  var pinnedCount = element("span", "section-count");
  pinnedHeading.appendChild(pinnedCount);
  var pinnedList = element("div", "pin-list");
  pinnedSection.appendChild(pinnedHeading);
  pinnedSection.appendChild(pinnedList);

  var unpinnedSection = element("section", "game-section");
  var unpinnedHeading = element("button", "section-heading");
  unpinnedHeading.type = "button";
  unpinnedHeading.appendChild(element("span", "section-chevron", "\u25be"));
  unpinnedHeading.appendChild(element("span", "section-label", "Unpinned"));
  var unpinnedCount = element("span", "section-count");
  unpinnedHeading.appendChild(unpinnedCount);
  var unpinnedList = element("div", "pin-list");
  unpinnedSection.appendChild(unpinnedHeading);
  unpinnedSection.appendChild(unpinnedList);

  var cards = [];
  managedGames.forEach(function (game) {
    var pin = pins[String(game.appid)];
    var card = gameCard(context, game, pin);
    cards.push({
      card: card,
      pinned: !!pin,
      search: (String(game.title || "") + " " + String(game.appid)).toLowerCase()
    });
    (pin ? pinnedList : unpinnedList).appendChild(card);
  });

  var pinnedEmpty = element("div", "empty compact", "No pinned games.");
  var unpinnedEmpty = element("div", "empty compact", "No unpinned games.");
  pinnedList.appendChild(pinnedEmpty);
  unpinnedList.appendChild(unpinnedEmpty);
  root.appendChild(pinnedSection);
  root.appendChild(unpinnedSection);

  function makeCollapsible(section, heading, list) {
    var collapsed = false;
    heading.addEventListener("click", function () {
      collapsed = !collapsed;
      section.classList.toggle("collapsed", collapsed);
      list.hidden = collapsed;
      heading.querySelector(".section-chevron").textContent =
        collapsed ? "\u25b8" : "\u25be";
    });
  }
  makeCollapsible(pinnedSection, pinnedHeading, pinnedList);
  makeCollapsible(unpinnedSection, unpinnedHeading, unpinnedList);

  function filterGames() {
    var query = search.value.trim().toLowerCase();
    var visiblePinned = 0;
    var visibleUnpinned = 0;
    cards.forEach(function (item) {
      var visible = !query || item.search.indexOf(query) !== -1;
      item.card.hidden = !visible;
      if (visible && item.pinned) visiblePinned += 1;
      if (visible && !item.pinned) visibleUnpinned += 1;
    });
    pinnedCount.textContent = String(visiblePinned);
    unpinnedCount.textContent = String(visibleUnpinned);
    pinnedEmpty.hidden = visiblePinned !== 0;
    unpinnedEmpty.hidden = visibleUnpinned !== 0;
  }
  search.addEventListener("input", filterGames);
  filterGames();
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
