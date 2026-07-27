var button = document.getElementById("collect");
var status = document.getElementById("status");
button.addEventListener("click", function () {
  button.disabled = true;
  status.textContent = "Collecting visible builds…";
  chrome.runtime.sendMessage({ action: "collect" }, function (reply) {
    button.disabled = false;
    if (chrome.runtime.lastError) {
      status.textContent = chrome.runtime.lastError.message;
    } else if (!reply || !reply.ok) {
      status.textContent = reply && reply.error || "Collection failed";
    } else {
      status.textContent = "Exported " + reply.value.count + " builds.";
    }
  });
});
