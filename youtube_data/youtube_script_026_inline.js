window.ytplayer = window.ytplayer || {};
window.ytplayer.bootstrapPlayerContainer = document.getElementById('player-api');
window.ytplayer.bootstrapWebPlayerContextConfig = window.yt && window.yt.config_ && window.yt.config_.WEB_PLAYER_CONTEXT_CONFIGS && window.yt.config_.WEB_PLAYER_CONTEXT_CONFIGS['WEB_PLAYER_CONTEXT_CONFIG_ID_KEVLAR_WATCH'];
window.ytplayer.bootstrapPlayerResponse = window['ytInitialPlayerResponse'];
(function playerBootstrap() {
  if (window.ytplayer.bootstrapPlayerContainer && window.ytplayer.bootstrapWebPlayerContextConfig) {
    var createPlayer = window.yt && window.yt.player && window.yt.player.Application && (window.yt.player.Application.createAlternate || window.yt.player.Application.create);
    if (createPlayer) {
      if (window.ytplayer.bootstrapPlayerResponse) {
        window.ytplayer.config = {
          args: {
            raw_player_response: window.ytplayer.bootstrapPlayerResponse
          }
        };
        if (window.ytcsi) window.ytcsi.tick("cfg", null, "")
      }
      createPlayer(window.ytplayer.bootstrapPlayerContainer,
        window.ytplayer.config, window.ytplayer.bootstrapWebPlayerContextConfig);
      window.pis = "initialized"
    }
  }
})();
ytplayer.load = function() {
  throw new Error("Unexpected call to ytplayer.load.");
};