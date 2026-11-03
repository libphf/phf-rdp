# PHF-RDP (Pereira-Huffman Format Remote Desktop)

Servidor de acesso remoto (RDP) e streaming de área de trabalho em tempo real a **60 FPS**, 100% sem perdas (*lossless*), construído em C nativo com **DXGI Desktop Duplication**, **Delta Sub-Frames**, **SIMD (SSSE3 + WebAssembly SIMD128)** e **Áudio WASAPI Loopback**.

---

## 🚀 Funcionalidades Completas (v1.2)

1. **Delta Frames (`Dirty Sub-Frame` — Opcode `0x06`)**:
   - Detecção vetorial ultra-rápida (~0,18 ms) do retângulo alterado entre frames.
   - Comprime e transmite apenas a região que mudou na tela `(rx, ry, rw, rh)` e faz upload parcial no `<canvas>` do navegador (`putImageData` sujo), reduzindo o tempo de encode/decode e o consumo de banda em **até 90%** durante o uso normal de desktop.
2. **Escalonamento Inteligente (`Auto`, `720p`, `1080p`, `Nativo`)**:
   - **Zero Upscale no Servidor**: O servidor nunca envia resolução maior que a do monitor de origem.
   - **Modo Janela**: Se a janela do cliente for menor que o monitor de origem, o servidor faz downscale bilinear `16.16 fixed-point` mantendo o *aspect ratio* exato.
   - **Modo Fullscreen / Telas Maiores**: A GPU do navegador faz upscale mantendo a proporção da imagem a custo zero de CPU.
3. **Controle Remoto RDP de Ultra-Baixa Latência (`TCP_NODELAY` + `SendInput`)**:
   - **Mouse sem Offset e sem Cursor Duplo**: Posicionamento físico exato e ocultação automática do cursor de software do vídeo quando o controle RDP está ativo.
   - **Teclado Independente de Layout (`KEYEVENTF_UNICODE` + `VK_*`)**: Acentos (`é`, `ç`, `ê`, `ã`, `~e`) são compostos nativamente no layout do cliente e injetados via Unicode no servidor.
4. **Clipboard Bidirecional (`Ctrl+C` ↔ `Ctrl+V` — Opcode `0x05`)**:
   - Pressionar `Ctrl+V` no cliente envia o texto UTF-8 ao servidor, atualiza o Clipboard do Windows (`CF_UNICODETEXT`) e cola imediatamente na janela remota ativa.
   - Copiar texto no servidor (`Ctrl+C`) sincroniza automaticamente com a área de transferência do cliente.
5. **Múltiplos Monitores e Suporte a Monitor Virtual (`IddCx`)**:
   - Enumera todos os monitores físicos e virtuais (como Parsec VDD / MTT Virtual Display Driver) conectados à GPU e permite alternar entre eles em tempo real pelo HUD.
6. **Áudio do Sistema em Tempo Real (`WASAPI Loopback` — Opcode `0x02`)**:
   - Captura nativa em C do áudio reproduzido no servidor com supressão automática de silêncio (`0 kbps` quando mudo) e reprodução via Web Audio API no navegador.

---

## 🛠️ Como Compilar e Executar

### 1. Compilação do Servidor (Windows MSVC):
```cmd
cd phf-rdp\server
build.bat
```

### 2. Inicialização:
```cmd
cd phf-rdp
run.bat
```

Acesse no navegador: **http://localhost:8080**
- `H` (ou `Alt + H` com controle RDP ativo): Exibir / Ocultar HUD
- `F` / `F11`: Tela Cheia
