(function () {
  function waitFor(test, timeout) {
    return new Promise(function (resolve, reject) {
      var started = Date.now();
      (function poll() {
        var value;
        try { value = test(); }
        catch (error) { reject(error); return; }
        if (value) return resolve(value);
        if (Date.now() - started > timeout) return reject(new Error("SteamDB content did not finish loading"));
        setTimeout(poll, 350);
      }());
    });
  }

  function appList() {
    var match = location.pathname.match(/^\/app\/(\d+)\/patchnotes\/?$/);
    if (!match) throw new Error("Open a SteamDB app Patches page first");
    return waitFor(function () {
      var builds = [];
      var buildTable = Array.prototype.find.call(
        document.querySelectorAll("table"),
        function (table) {
          return Array.prototype.some.call(table.querySelectorAll("th"),
            function (heading) { return heading.textContent.trim() === "BuildID"; });
        });
      if (!buildTable) return null;
      var headings = Array.prototype.map.call(buildTable.querySelectorAll("th"),
        function (heading) { return heading.textContent.trim().toLowerCase(); });
      var dateIndex = headings.indexOf("date");
      var timeIndex = headings.indexOf("time");
      if (dateIndex < 0 || timeIndex < 0)
        throw new Error("Builds table is missing Date or Time column");
      buildTable.querySelectorAll('a[href^="/patchnotes/"]').forEach(function (link) {
        var found = link.getAttribute("href").match(/^\/patchnotes\/(\d+)\/?$/);
        if (!found || builds.some(function (item) { return item.build_id === found[1]; })) return;
        var row = link.closest("tr");
        var cells = row && row.querySelectorAll("td");
        var dateText = cells && cells[dateIndex] && cells[dateIndex].innerText.trim();
        var timeText = cells && cells[timeIndex] && cells[timeIndex].innerText.trim();
        var instant = new Date(dateText + " " + timeText + " UTC");
        if (!dateText || !timeText || isNaN(instant.getTime()))
          throw new Error("Build " + found[1] + " has invalid Date/Time cells");
        builds.push({
          build_id: found[1],
          published_at: instant.toISOString()
        });
      });
      return builds.length ? { appid: match[1], builds: builds } : null;
    }, 20000);
  }

  function dlcList() {
    var match = location.pathname.match(/^\/app\/(\d+)\/dlc\/?$/);
    if (!match) throw new Error("Not a SteamDB DLC list");
    return waitFor(function () {
      var table = Array.prototype.find.call(document.querySelectorAll("table"),
        function (candidate) {
          return Array.prototype.some.call(candidate.querySelectorAll("th"),
            function (heading) { return heading.textContent.trim() === "AppID"; });
        });
      if (!table) return null;
      var appids = [];
      table.querySelectorAll('a[href^="/app/"]').forEach(function (link) {
        var found = link.getAttribute("href").match(/^\/app\/(\d+)\/?$/);
        if (found && found[1] !== match[1] && appids.indexOf(found[1]) < 0)
          appids.push(found[1]);
      });
      return { appid: match[1], dlc_appids: appids };
    }, 10000);
  }

  function isoTime(element) {
    if (!element) return null;
    var value = element.getAttribute("datetime") || element.getAttribute("data-time");
    if (!value) return null;
    var instant = new Date(value);
    return isNaN(instant.getTime()) ? null : instant.toISOString();
  }

  function depotMap() {
    var match = location.pathname.match(/^\/app\/(\d+)\/depots\/?$/);
    if (!match) throw new Error("Not a SteamDB depot page");
    return waitFor(function () {
      var scope = document.querySelector(".scope-app[data-appid]");
      return scope && scope.getAttribute("data-appid") === match[1] ? scope : null;
    }, 10000).then(function () {
      var pane = document.querySelector("#depots");
      var sections = pane && Array.prototype.filter.call(pane.children,
        function (child) {
          return child.tagName === "H2" && child.nextElementSibling &&
            child.nextElementSibling.tagName === "TABLE";
        });
      if (!sections || !sections.some(function (heading) {
        return heading.textContent.trim().toLowerCase() === "depots";
      })) {
        return { appid: match[1], downloadable: false, depots: [], branches: [] };
      }
      var depots = [];
      function categoryRank(category) {
        return category === "dlc" ? 4 : category === "redistributable" ? 3 :
          category === "shared" ? 2 : 1;
      }
      function recordDepot(candidate, appearance) {
        var existing = depots.find(function (item) {
          return item.depot_id === candidate.depot_id &&
            item.owner_appid === candidate.owner_appid;
        });
        if (!existing) {
          candidate.appearances = [appearance];
          depots.push(candidate);
          return;
        }
        existing.appearances.push(appearance);
        if (categoryRank(candidate.category) > categoryRank(existing.category)) {
          existing.category = candidate.category;
          existing.section = candidate.section;
          existing.descriptor = candidate.descriptor;
          existing.configuration = candidate.configuration;
          existing.size = candidate.size;
          existing.dl = candidate.dl;
        }
      }
      sections.forEach(function (heading) {
        var sectionName = heading.textContent.trim();
        var sectionCategory = sectionName.toLowerCase() === "depots" ? "content" :
          sectionName.toLowerCase() === "redistributables" ? "redistributable" :
          sectionName.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
        heading.nextElementSibling.querySelectorAll(
          "tbody tr.depot[data-depotid]"
        ).forEach(function (row) {
        var depot = row.getAttribute("data-depotid");
        var ownerLink = row.querySelector("a.app[data-appid]");
        var configurationCell = row.querySelector(".depot-config");
        var configurationParts = configurationCell && Array.prototype.map.call(
          configurationCell.children,
          function (part) { return part.innerText.trim(); }
        ).filter(Boolean);
        function quantity(selector) {
          var cell = row.querySelector(selector);
          if (!cell) return { bytes: null, display: null };
          var bytes = cell.getAttribute("data-sort");
          return {
            bytes: bytes && /^\d+$/.test(bytes) ? bytes : null,
            display: cell.innerText.trim() || null
          };
        }
        var configuration = configurationParts && configurationParts.length ?
          configurationParts.join(" · ") :
          (configurationCell || row).innerText.trim();
        var descriptorNode = configurationCell &&
          configurationCell.querySelector(".i.muted");
        var category = sectionCategory;
        if (category === "content" && ownerLink &&
            ownerLink.getAttribute("data-appid") !== match[1]) {
          category = /^DLC\s+\d+/i.test(ownerLink.innerText.trim()) ? "dlc" : "shared";
        }
        var candidate = {
          depot_id: depot,
          owner_appid: ownerLink ? ownerLink.getAttribute("data-appid") : match[1],
          category: category,
          section: sectionName,
          descriptor: descriptorNode ? descriptorNode.innerText.trim() : configuration,
          configuration: configuration,
          size: quantity(".depot-size-disk"),
          dl: quantity(".depot-size-download")
        };
        recordDepot(candidate, {
          category: category,
          section: sectionName,
          descriptor: candidate.descriptor,
          configuration: configuration,
          size: candidate.size,
          dl: candidate.dl
        });
        });
      });
      var branches = [];
      var branchHeading = document.querySelector("#branches");
      var branchTable = branchHeading && branchHeading.nextElementSibling;
      if (branchTable && branchTable.tagName === "TABLE") {
        branchTable.querySelectorAll("tbody tr").forEach(function (row) {
          var cells = row.querySelectorAll("td");
          var buildLink = row.querySelector('a[href^="/patchnotes/"]');
          var build = buildLink && buildLink.getAttribute("href").match(/^\/patchnotes\/(\d+)\/?$/);
          var times = row.querySelectorAll("[datetime]");
          if (!cells.length || !build) return;
          branches.push({
            name: cells[0].innerText.trim(),
            build_id: build[1],
            built_at: isoTime(times[0]),
            updated_at: isoTime(times[1])
          });
        });
      }
      return {
        appid: match[1], downloadable: true,
        depots: depots, branches: branches
      };
    });
  }

  function depotManifests() {
    var match = location.pathname.match(/^\/depot\/(\d+)\/manifests\/?$/);
    if (!match) throw new Error("Not a SteamDB depot manifests page");
    return waitFor(function () {
      return Array.prototype.find.call(document.querySelectorAll("h1,h2,h3"),
        function (heading) {
          return heading.textContent.trim().toLowerCase() === "previously seen manifests";
        });
    }, 10000).then(function (heading) {
      var scope = heading.parentElement || document;
      var table = Array.prototype.find.call(scope.querySelectorAll("table"),
        function (candidate) {
          return Array.prototype.some.call(candidate.querySelectorAll("th"),
            function (cell) { return cell.textContent.trim() === "ManifestID"; });
        });
      if (!table) throw new Error("Previously seen manifests table is missing");
      var manifests = [];
      table.querySelectorAll("tbody tr").forEach(function (row) {
        var cells = row.querySelectorAll("td");
        var manifest = Array.prototype.map.call(row.querySelectorAll("a"),
          function (link) { return link.textContent.trim(); }
        ).find(function (text) { return /^\d{10,20}$/.test(text); });
        if (!manifest || manifests.some(function (item) {
          return item.manifest_id === manifest;
        })) return;
        var time = row.querySelector("[datetime], [data-time]");
        manifests.push({
          manifest_id: manifest,
          seen_at: isoTime(time),
          seen_display: cells[0] ? cells[0].innerText.trim() : null
        });
      });
      if (!manifests.length) throw new Error("No previously seen manifests were rendered");
      return { depot_id: match[1], manifests: manifests };
    });
  }

  function patch(terminal) {
    var match = location.pathname.match(/^\/patchnotes\/(\d+)\/?$/);
    if (!match) throw new Error("Not a SteamDB patch page");
    return waitFor(function () {
      var text = document.body.innerText;
      var lower = text.toLowerCase();
      if (lower.indexOf(("Build " + match[1]).toLowerCase()) < 0) return false;
      return terminal || lower.indexOf("changed files in this update") >= 0;
    }, 10000).then(function () {
      var initialText = document.body.innerText;
      if (initialText.toLowerCase().indexOf("changed files in this update") < 0) {
        if (terminal) return initialText;
        throw new Error("Non-terminal build has no Changed Files section");
      }
      var changedHeading = Array.prototype.find.call(
        document.querySelectorAll("h1,h2,h3,h4"),
        function (heading) {
          return heading.textContent.toLowerCase().indexOf(
            "changed files in this update") >= 0;
        });
      if (changedHeading) changedHeading.scrollIntoView({ block: "start" });
      else window.scrollTo(0, document.body.scrollHeight);
      return waitFor(function () {
        var text = document.body.innerText;
        var changedAt = text.toLowerCase().indexOf("changed files in this update");
        if (changedAt < 0) return null;
        var changed = text.slice(changedAt);
        var previous = changed.toLowerCase().indexOf("previous update");
        if (previous >= 0) changed = changed.slice(0, previous);
        return !/loading history(?:…|\.\.\.)/i.test(changed) ? text : null;
      }, 10000);
    }).then(function (text) {
      var transitions = {};
      var events = [];
      var changedAt = text.toLowerCase().indexOf("changed files in this update");
      if (changedAt < 0 && terminal) {
        return { build_id: match[1], transitions: transitions, events: events };
      }
      var changed = text.slice(changedAt);
      var previous = changed.toLowerCase().indexOf("previous update");
      if (previous >= 0) changed = changed.slice(0, previous);
      var headings = [];
      var depotPattern = /\bDepot\s+(\d+)\b/gi;
      var depotMatch;
      while ((depotMatch = depotPattern.exec(changed))) {
        headings.push({ id: depotMatch[1], at: depotMatch.index });
      }
      headings.forEach(function (heading, index) {
        var section = changed.slice(heading.at,
          index + 1 < headings.length ? headings[index + 1].at : changed.length);
        var change = section.match(
          /Manifest ID changed\s*[–-]\s*(\d+)\s*[›>]\s*(\d+)/i);
        var event = {
          depot_id: heading.id,
          kind: change ? "manifest_changed" :
            /(?:depot\s+)?added/i.test(section) ? "depot_added" :
            /(?:depot\s+)?removed/i.test(section) ? "depot_removed" : "other",
          raw: section.trim()
        };
        if (change) {
          event.old = change[1];
          event.new = change[2];
          transitions[heading.id] = { old: change[1], new: change[2] };
        }
        events.push(event);
      });
      return { build_id: match[1], transitions: transitions, events: events };
    }).catch(function (error) {
      var text = document.body.innerText;
      var changedAt = text.toLowerCase().indexOf("changed files in this update");
      var excerpt = changedAt >= 0 ? text.slice(changedAt, changedAt + 1200) :
        text.slice(0, 1200);
      throw new Error(error.message + "\n\nRendered page excerpt:\n" + excerpt);
    });
  }

  function status(message) {
    var panel = document.getElementById("ronin-steamdb-collector-status");
    if (!panel) {
      panel = document.createElement("pre");
      panel.id = "ronin-steamdb-collector-status";
      panel.style.cssText = [
        "position:fixed", "right:16px", "bottom:16px", "z-index:2147483647",
        "max-width:560px", "max-height:45vh", "overflow:auto", "margin:0",
        "padding:12px", "white-space:pre-wrap", "background:#171a21",
        "color:#c7d5e0", "border:1px solid #66c0f4", "border-radius:5px",
        "font:12px/1.4 monospace", "box-shadow:0 4px 24px #000"
      ].join(";");
      document.documentElement.appendChild(panel);
    }
    panel.textContent = String(message);
    return {};
  }

  chrome.runtime.onMessage.addListener(function (message, sender, respond) {
    var task;
    try {
      task = message.action === "status" ? Promise.resolve(status(message.message)) :
        message.action === "depots" ? depotMap() :
        message.action === "depot_manifests" ? depotManifests() :
        message.action === "dlcs" ? dlcList() :
        message.action === "list" ? appList() :
        message.action === "patch" ? patch(!!message.terminal) :
        Promise.reject(new Error("Unknown collector action"));
    } catch (error) {
      task = Promise.reject(error);
    }
    task.then(function (value) { respond({ ok: true, value: value }); },
      function (error) { respond({ ok: false, error: String(error.message || error) }); });
    return true;
  });
}());
