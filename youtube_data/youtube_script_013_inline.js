window.ytAtP = new Promise(res => window.ytAtN = res);
window.addEventListener('DOMContentLoaded', () => {
  window.ytAtN();
  delete window.ytAtN;
});