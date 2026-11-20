/**
 * @name iTunes 公开预览示例
 * @description LXMusicC 扩展搜索示例。仅返回 Apple 提供的短预览，不是完整歌曲；无歌词。
 * @version 1.0.0
 * @author LXMusicC
 * @homepage https://performance-partners.apple.com/search-api
 */
const { EVENT_NAMES, request, on, send, utils } = globalThis.lx;
function http(url, options = {}) {
  return new Promise((resolve, reject) => {
    request(url, options, (error, response) => {
      if (error) return reject(error);
      if (response.statusCode < 200 || response.statusCode >= 300) {
        return reject(new Error('HTTP ' + response.statusCode + '，请稍后重试'));
      }
      resolve(response.body);
    });
  });
}
on(EVENT_NAMES.request, async ({ action, info }) => {
  switch (action) {
    // search 和 itunes provider 是 LXMusicC 扩展，不属于 LX 官方动作/平台集合。
    case 'search': {
      const limit = Math.min(30, Math.max(1, Number(info.limit) || 5));
      const data = await http('https://itunes.apple.com/search', {
        qs: { term: info.keyword, entity: 'song', country: 'us', limit }, timeout: 15000, json: true
      });
      if (!Array.isArray(data.results)) throw new Error('服务器没有返回 results 数组');
      return data.results.filter(song => song.previewUrl).map(song => ({
        // 示例真实使用 buffer 的 UTF-8/base64 互转，保持原始 trackId。
        id: utils.buffer.bufToString(utils.buffer.from(
          utils.buffer.bufToString(utils.buffer.from(String(song.trackId)), 'base64'), 'base64')),
        name: song.trackName,
        artist: song.artistName,
        album: song.collectionName || '',
        duration: (song.trackTimeMillis || 0) / 1000,
        quality: 'preview-aac',
        url: song.previewUrl,
        cover: song.artworkUrl100 || ''
      }));
    }
    case 'musicUrl':
      if (!info.musicInfo || !info.musicInfo.url) throw new Error('缺少预览地址');
      // 实际验证地址可访问；HEAD 不下载音频，不提前实现缓存/播放。
      await http(info.musicInfo.url, { method: 'HEAD', timeout: 15000 });
      return info.musicInfo.url;
    case 'lyric': return { lyric: '' }; // Apple Search API 不提供歌词，不伪造内容。
    default: throw new Error('不支持的 action: ' + action);
  }
});
// 验证初始化阶段 timer + Promise 的事件循环；导入时不会执行。
setTimeout(() => send(EVENT_NAMES.inited, {
  sources: { itunes: { name: 'iTunes Preview', type: 'music',
    actions: ['search', 'musicUrl', 'lyric'], qualitys: ['preview-aac'] } }
}), 0);
