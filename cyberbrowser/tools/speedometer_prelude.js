// Evaluated in Speedometer's page before its scripts (CYBER_PRELUDE_JS) by
// tests/jit/speedometer.sh: reports the run's result on the console and sets
// __cyberDone, which ends the browser's event loop (CYBER_RUN_UNTIL).
//   SPEEDOMETER-DONE {"<suite>": {mean, unit}, ..., "Geomean": ..., "Score": ...}
// (means only: the console splits lines past about 2 KB)
//   SPEEDOMETER-ERROR <message>
(function () {
    function done(line) {
        console.error(line);
        globalThis.__cyberDone = true;
    }
    addEventListener('SpeedometerDone', function () {
        var c = globalThis.benchmarkClient, m = c && c._metrics, out = {};
        for (var k in m) {
            var metric = m[k];
            /* the suites' totals, Geomean and Score; not iterations or steps */
            if (!metric || metric.name.indexOf('Iteration-') === 0 || metric.name.indexOf('/') >= 0) continue;
            out[metric.name] = { mean: Math.round(metric.mean * 1000) / 1000, unit: metric.unit };
        }
        done('SPEEDOMETER-DONE ' + JSON.stringify(out));
    });
    addEventListener('error', function (e) {
        done('SPEEDOMETER-ERROR ' + (e && (e.message || (e.error && e.error.message))) +
             (e && e.error && e.error.stack ? ' | ' + e.error.stack : ''));
    });
    addEventListener('unhandledrejection', function (e) {
        var r = e && e.reason;
        done('SPEEDOMETER-ERROR unhandled rejection: ' + (r && (r.message || r)) + (r && r.stack ? ' | ' + r.stack : ''));
    });
})();
