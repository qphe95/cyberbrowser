var t0 = Date.now(), count = 0;
function tick() {
  count++;
  if (count < 5) requestAnimationFrame(tick);
  else console.error("RAF done 5 ticks in " + (Date.now() - t0) + "ms");
}
requestAnimationFrame(tick);
setTimeout(function(){ console.error("after 300ms: count=" + count); }, 300);
