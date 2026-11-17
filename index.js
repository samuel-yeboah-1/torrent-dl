#!/usr/bin/env node
import WebTorrent from 'webtorrent'
import readline from 'node:readline/promises'
import os from 'node:os'
import path from 'node:path'

const downloadDir = path.join(os.homedir(), 'Downloads')

async function askForLink() {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout })
  const link = await rl.question('Paste magnet link: ')
  rl.close()
  return link.trim()
}

function formatBytes(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let i = 0
  while (bytes >= 1024 && i < units.length - 1) {
    bytes /= 1024
    i++
  }
  return `${bytes.toFixed(1)} ${units[i]}`
}

function drawProgress(torrent) {
  const width = 30
  const filled = Math.round(torrent.progress * width)
  const bar = '█'.repeat(filled) + '░'.repeat(width - filled)
  const percent = (torrent.progress * 100).toFixed(1).padStart(5)
  const speed = `${formatBytes(torrent.downloadSpeed)}/s`
  const peers = `${torrent.numPeers} peers`
  process.stdout.write(`\r${bar} ${percent}%  ${speed}  ${peers}   `)
}

const link = process.argv[2] || (await askForLink())

if (!link.startsWith('magnet:')) {
  console.error('That does not look like a magnet link.')
  process.exit(1)
}

const client = new WebTorrent()
client.on('error', (err) => {
  console.error(`\nError: ${err.message}`)
  process.exit(1)
})

console.log('Finding peers...')

client.add(link, { path: downloadDir }, (torrent) => {
  console.log(`Downloading: ${torrent.name} (${formatBytes(torrent.length)})`)

  const timer = setInterval(() => drawProgress(torrent), 500)

  torrent.on('done', () => {
    clearInterval(timer)
    drawProgress(torrent)
    console.log(`\nDone. Saved to ${path.join(downloadDir, torrent.name)}`)
    client.destroy(() => process.exit(0))
  })
})

process.on('SIGINT', () => {
  console.log('\nStopped.')
  client.destroy(() => process.exit(0))
})
