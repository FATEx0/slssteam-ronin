var root;

function element(tag, className, text) {
  var node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function parseDepots(text) {
  var depots = {};
  String(text || "").split(/\r?\n/).forEach(function (line, index) {
    if (!line.trim()) return;
    var match = line.match(/^\s*(\d+)\s*=\s*(\d+)\s*$/);
    if (!match) throw new Error("Depot line " + (index + 1) + " must be DEPOT_ID = MANIFEST_GID");
    if (depots[match[1]] != null) throw new Error("Duplicate depot " + match[1]);
    depots[match[1]] = match[2];
  });
  if (!Object.keys(depots).length) throw new Error("Add at least one depot manifest.");
  return depots;
}

function rootBuilds(history) {
  if (!history || typeof history !== "object") return [];
  if (Array.isArray(history.apps)) {
    var rootHistory = history.apps.find(function (app) {
      return String(app.appid) === String(history.appid);
    });
    return rootHistory && Array.isArray(rootHistory.builds) ? rootHistory.builds : [];
  }
  return Array.isArray(history.builds) ? history.builds : [];
}

function importHistory(context, game, file) {
  return file.text().then(function (text) {
    var observation = JSON.parse(text);
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
  });
}

function pinEditor(context, game, app, shownDepots, onClose) {
  var shell = element("div", "pin-editor-shell");
  var form = element("div", "pin-form pin-editor");
  var appid = element("input"); appid.placeholder = "App ID"; appid.inputMode = "numeric";
  var buildControl = element("div", "build-control");
  var build = element("select");
  var manualBuild = element("input");
  manualBuild.placeholder = "Build ID (0 if unknown)";
  manualBuild.inputMode = "numeric";
  manualBuild.hidden = true;
  buildControl.appendChild(build);
  buildControl.appendChild(manualBuild);
  var historyFile = element("input");
  historyFile.type = "file";
  historyFile.accept = "application/json,.json";
  historyFile.hidden = true;
  var depots = element("textarea"); depots.placeholder = "Depot ID = Manifest GID";
  var lockedLabel = element("label", "locked");
  var locked = element("input"); locked.type = "checkbox";
  lockedLabel.appendChild(locked); lockedLabel.appendChild(document.createTextNode(" Lock Steam update reconciliation to this build"));
  var save = element("button", "primary", "Save pin"); save.type = "button";
  var resolve = element("button", "", "Resolve build"); resolve.type = "button";
  var cancel = element("button", "", "Cancel"); cancel.type = "button";
  var message = element("div", "message");

  appid.value = game.appid;
  var initialBuild = String(app ? app.build_id : (game.installed_build_id || "0"));
  manualBuild.value = initialBuild;
  locked.checked = app ? !!app.locked : true;
  depots.value = Object.keys(shownDepots).map(function (id) {
    return id + " = " + shownDepots[id];
  }).join("\n");

  var buttons = element("div", "editor-actions");
  buttons.appendChild(save); buttons.appendChild(resolve); buttons.appendChild(cancel);
  form.appendChild(appid); form.appendChild(buildControl);
  form.appendChild(historyFile); form.appendChild(depots);
  form.appendChild(lockedLabel); form.appendChild(buttons); form.appendChild(message);

  function addBuildOption(value, text) {
    var option = element("option", "", text);
    option.value = value;
    build.appendChild(option);
  }

  function selectedBuild() {
    return build.value === "__manual__" ? manualBuild.value.trim() : build.value;
  }

  function setBuildChoices(history, preferred) {
    var builds = rootBuilds(history);
    build.textContent = "";
    builds.forEach(function (item) {
      var label = String(item.build_id);
      if (item.published_at) {
        var date = new Date(item.published_at);
        label += " — " + (isNaN(date.getTime()) ? item.published_at :
          date.toLocaleString());
      }
      addBuildOption(String(item.build_id), label);
    });
    addBuildOption("__load__", "Load history file…");
    addBuildOption("__manual__", "Enter build manually…");

    var wanted = String(preferred || "");
    var known = builds.some(function (item) {
      return String(item.build_id) === wanted;
    });
    if (known) {
      build.value = wanted;
      manualBuild.hidden = true;
    } else {
      build.value = "__manual__";
      manualBuild.value = wanted || "0";
      manualBuild.hidden = false;
    }
    return builds;
  }

  var previousBuildChoice = "__manual__";
  build.addEventListener("change", function () {
    if (build.value === "__load__") {
      build.value = previousBuildChoice;
      historyFile.value = "";
      historyFile.click();
      return;
    }
    previousBuildChoice = build.value;
    manualBuild.hidden = build.value !== "__manual__";
    if (!manualBuild.hidden) manualBuild.focus();
  });

  historyFile.addEventListener("change", function () {
    var file = historyFile.files && historyFile.files[0];
    if (!file) {
      setBuildChoices(null, manualBuild.value || initialBuild);
      return;
    }
    message.textContent = "Importing history…";
    build.disabled = true;
    importHistory(context, game, file).then(function (history) {
      var builds = setBuildChoices(history, game.installed_build_id || initialBuild);
      previousBuildChoice = build.value;
      message.textContent = "Cached " + builds.length + " builds.";
      build.disabled = false;
    }, function (error) {
      setBuildChoices(null, manualBuild.value || initialBuild);
      message.textContent = String(error && error.message || error);
      build.disabled = false;
    });
  });

  build.disabled = true;
  addBuildOption("", "Loading build history…");
  context.call("pins.history.list", { appid: String(game.appid) }).then(function (history) {
    setBuildChoices(history, initialBuild);
    previousBuildChoice = build.value;
    build.disabled = false;
  }, function () {
    setBuildChoices(null, initialBuild);
    previousBuildChoice = build.value;
    build.disabled = false;
  });

  save.addEventListener("click", function () {
    message.textContent = "";
    try {
      var payload = {
        appid: appid.value.trim(),
        build_id: selectedBuild() || "0",
        locked: locked.checked,
        depots: parseDepots(depots.value)
      };
      save.disabled = true;
      context.call("pins.set", payload).then(function (next) {
        render(context, next);
      }, function (error) {
        message.textContent = String(error && error.message || error);
        save.disabled = false;
      });
    } catch (error) {
      message.textContent = error.message;
    }
  });
  resolve.addEventListener("click", function () {
    message.textContent = "";
    resolve.disabled = true;
    context.call("pins.history.resolve", {
      appid: appid.value.trim(), build_id: selectedBuild()
    }).then(function (result) {
      depots.value = Object.keys(result.depots || {}).map(function (id) {
        return id + " = " + result.depots[id];
      }).join("\n");
      message.textContent = "Resolved from validated cached history.";
      resolve.disabled = false;
    }, function (error) {
      message.textContent = String(error && error.message || error);
      resolve.disabled = false;
    });
  });
  cancel.addEventListener("click", onClose);
  shell.appendChild(form);
  return { shell: shell, focus: function () { build.focus(); } };
}

function render(context, state) {
  state = state || {};
  // Tsuki's Lua JSON bridge cannot distinguish an empty array from an empty
  // object after decoding and re-encoding a module response. Treat either
  // non-array shape as the intended empty collection at this view boundary.
  var configuredApps = Array.isArray(state.apps) ? state.apps : [];
  var managedGames = Array.isArray(state.games) ? state.games : [];
  root.textContent = "";
  var intro = element("div", "intro");
  intro.appendChild(element("h1", "", "Manifest Pins"));
  intro.appendChild(element("p", "", "Lock an added game to exact depot manifest versions. Changes are written to SLSsteam’s configuration and are observed by Ronin at runtime."));
  root.appendChild(intro);

  var pins = {};
  configuredApps.forEach(function (pin) { pins[pin.appid] = pin; });
  var games = managedGames.slice();
  configuredApps.forEach(function (pin) {
    if (!games.some(function (game) { return game.appid === pin.appid; })) {
      games.push({
        appid: pin.appid, title: "App " + pin.appid,
        installed_build_id: pin.build_id, manifests: pin.depots
      });
    }
  });
  games.sort(function (a, b) { return a.title.localeCompare(b.title); });

  var heading = element("h2", "", "Managed games");
  root.appendChild(heading);
  var list = element("div", "pin-list");
  games.forEach(function (game) {
    var app = pins[game.appid];
    var shownDepots = app ? app.depots : (game.manifests || {});
    var card = element("section", "pin-card");
    var title = element("div", "pin-title");
    var summary = element("div", "game-summary");
    var artwork = element("img", "game-art");
    artwork.alt = "";
    artwork.draggable = false;
    artwork.src = "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/"
      + encodeURIComponent(game.appid) + "/header.jpg";
    artwork.addEventListener("error", function () { artwork.hidden = true; });
    var identity = element("div");
    identity.appendChild(element("strong", "", game.title || ("App " + game.appid)));
    identity.appendChild(element("small", "", "App " + game.appid));
    identity.appendChild(element("small", "build", app
      ? "Pinned build " + app.build_id
      : "Installed build " + (game.installed_build_id || "unknown")));
    summary.appendChild(artwork);
    summary.appendChild(identity);
    title.appendChild(summary);
    title.appendChild(element("span", app && app.locked ? "state locked-state" : "state",
      app ? (app.locked ? "Locked" : "Pinned") : "Available"));
    card.appendChild(title);
    var depotList = element("div", "depots");
    Object.keys(shownDepots).sort(function (a, b) { return Number(a) - Number(b); })
      .forEach(function (depot) {
        depotList.appendChild(element("code", "", depot + " → " + shownDepots[depot]));
      });
    if (!Object.keys(shownDepots).length) {
      depotList.appendChild(element("span", "muted", "No local manifest versions discovered."));
    }
    card.appendChild(depotList);
    var cardMessage = element("div", "message card-message");
    var edit = element("button", "", app ? "Edit pin" : "Pin this build");
    var editor = null;
    function closeEditor() {
      if (!editor) return;
      var closing = editor;
      editor = null;
      edit.textContent = app ? "Edit pin" : "Pin this build";
      closing.classList.remove("open");
      setTimeout(function () { closing.remove(); }, 180);
    }
    edit.addEventListener("click", function () {
      if (editor) { closeEditor(); return; }
      var built = pinEditor(context, game, app, shownDepots, closeEditor);
      editor = built.shell;
      card.appendChild(editor);
      edit.textContent = "Close";
      requestAnimationFrame(function () {
        if (!editor) return;
        editor.classList.add("open");
        built.focus();
      });
    });
    var actions = element("div", "actions"); actions.appendChild(edit);
    if (app) {
      var remove = element("button", "danger", "Remove pin");
      remove.addEventListener("click", function () {
        remove.disabled = true;
        context.call("pins.clear", { appid: app.appid }).then(function (next) {
          render(context, next);
        }, function (error) {
          cardMessage.textContent = String(error && error.message || error);
          remove.disabled = false;
        });
      });
      actions.appendChild(remove);
    }
    card.appendChild(actions); card.appendChild(cardMessage); list.appendChild(card);
  });
  if (!games.length) list.appendChild(element("div", "empty", "No managed games discovered."));
  root.appendChild(list);
}

function mount(context) {
  root = context.container;
  root.className = "manifest-pins-page";
  root.textContent = "Loading manifest pins…";
  context.call("pins.list", {}).then(function (state) {
    render(context, state);
  }, function (error) {
    root.textContent = "Unable to load manifest pins: " + String(error && error.message || error);
  });
}

function unmount() {
  if (root) root.textContent = "";
  root = null;
}
