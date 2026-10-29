// Development helper: QEMU (running inside WSL) starts this through
// `-netdev user,guestfwd=tcp:10.0.2.100:11434-cmd:node.exe host-relay.js PORT`
// for every connection the guest opens to 10.0.2.100:11434. It pipes the
// connection to 127.0.0.1:PORT on the Windows host, where a local model
// server (Ollama) listens — WSL's NAT networking cannot reach it otherwise.
const net = require('net');
const port = Number(process.argv[2] || 11434);
const sock = net.connect(port, '127.0.0.1');
process.stdin.pipe(sock);
sock.pipe(process.stdout);
sock.on('error', () => process.exit(1));
sock.on('close', () => process.exit(0));
process.stdin.on('end', () => sock.end());
