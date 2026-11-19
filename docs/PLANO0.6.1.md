# Plano de desempenho e gráficos: battride-war-recomp

**Versão alvo: 0.6.1.**

Repositório alvo: [robertoqimin/battride-war-recomp](https://github.com/robertoqimin/battride-war-recomp) (partindo da v0.6.0, SDK `sp00nznet/ps3recomp@a6d76c8`).
Referência: [SamiEnazi/infamous2-port](https://github.com/SamiEnazi/infamous2-port) (v0.6.3).

Este documento junta oito coisas:

1. Como o port do inFAMOUS 2 funciona, como referência.
2. O que trazer dele para o Battride: otimizações do lifter PPU e melhorias gráficas.
3. O desbloqueio de fps, para medir melhor.
4. Melhorias próprias do Battride, que não dependem do port do inFAMOUS 2.
5. Os problemas da instalação no Windows: o limite de 260 caracteres, os avisos por falta de assinatura e o
   Windows 10 sem teste.
6. As atualizações que hoje recompilam o jogo inteiro, de 15 a 60 minutos, mesmo quando só a documentação mudou, e
   por que a primeira compilação não pode sumir.
7. O tamanho dos pacotes: 707 MB no Windows e 235 MB no AppImage, contra cerca de 50 MB no infamous2.
8. A interface do launcher: as páginas passam das abas para uma sidebar à esquerda.

No fim há uma ordem única de execução.

> Os ganhos e prazos são estimativas, tiradas das proporções do `docs/PERFORMANCE.md` do Battride e do que o
> infamous2-port mediu no jogo dele. Nada aqui foi medido no Battride. Os itens que dependem de olhar o EBOOT ou de
> rodar o jogo estão marcados.

---

## Sumário

- [Parte 1: como o infamous2-port funciona](#parte-1-como-o-infamous2-port-funciona)
- [Parte 2: onde está o gargalo do Battride](#parte-2-onde-está-o-gargalo-do-battride)
- [Parte 3: regras de medição e adoção](#parte-3-regras-de-medição-e-adoção)
- [Parte 4: fase F, desbloqueio de fps e métrica de folga](#parte-4-fase-f-desbloqueio-de-fps-e-métrica-de-folga)
- [Parte 5: fases 0 a 5, otimizações do lifter vindas do infamous2](#parte-5-fases-0-a-5-otimizações-do-lifter-vindas-do-infamous2)
- [Parte 6: melhorias próprias do Battride (M1 a M10)](#parte-6-melhorias-próprias-do-battride-m1-a-m10)
- [Parte 7: fases G1 a G7, melhorias gráficas](#parte-7-fases-g1-a-g7-melhorias-gráficas)
- [Parte 8: o que não fazer](#parte-8-o-que-não-fazer)
- [Parte 9: instalação no Windows (W1 a W4)](#parte-9-instalação-no-windows-w1-a-w4)
- [Parte 10: atualizações sem recompilar tudo (U1 a U6)](#parte-10-atualizações-sem-recompilar-tudo-u1-a-u6)
- [Parte 11: tamanho dos pacotes (S1 a S3)](#parte-11-tamanho-dos-pacotes-s1-a-s3)
- [Parte 12: interface do launcher (L1)](#parte-12-interface-do-launcher-l1)
- [Parte 13: ordem única de execução](#parte-13-ordem-única-de-execução)

---

## Parte 1: como o infamous2-port funciona

Não é um emulador. É uma **recompilação estática**: o código PS3 do jogo é traduzido para C/C++ e compilado como um
programa nativo de PC, na máquina do jogador e a partir da cópia dele. A tradução acontece uma vez, na instalação.
Depois disso o jogo roda direto na CPU, com os gráficos em Vulkan. No PC de teste do autor, deu 3,4× o fps do RPCS3
no mesmo ponto do jogo.

### O que o Setup faz (`tools/setup_build.ps1`)

1. Verifica o PC e o jogo, que pode ser uma pasta, um `.iso` ou um `.pkg` da PSN com o `.rap`.
2. Baixa cerca de 48 MB de ferramentas: a engine pré-compilada, clang + lld e um Python embutido.
3. Copia o jogo para a pasta do port e baixa dos servidores da Sony o update 1.04 e o firmware do PS3.
4. Descriptografa o EBOOT 1.04 (`EBOOT_upd104.elf`).
5. Traduz o jogo para C/C++:
   - o código PPU, com `ppu_lifter.py`;
   - o código SPU, com `spu_lifter.py`;
   - três módulos de firmware da Sony (liblv2, libsysmodule, libsre), no modo "LLE".

   O clang compila tudo em `Infamous2Game.dll`, ao lado da engine.
6. Opcionalmente faz PGO: treina numa partida automática e recompila.

### O runtime (`engine/`)

- `runtime/`: memória do PS3, syscalls, threads PPU e SPU, PRX.
- `libs/`: as bibliotecas do sistema (arquivos, áudio, input, saves, SPURS).
- Vídeo: lê a FIFO de comandos do RSX e desenha em Vulkan em qualquer resolução. Os fragment shaders do RSX também
  são traduzidos.

### Jobs SPU nativos (`natives/`)

Os jobs SPU mais pesados (Edge geometry, render list e o clipper BSP) foram reescritos em C com AVX2 e verificados
bit a bit contra a versão traduzida. São específicos do inFAMOUS 2.

### Origem do código

| Pasta | O que é |
|---|---|
| `engine/` | Fork do ps3recomp de sp00nznet, mantido no próprio repositório |
| `base/` | O port original de Erik Martins (infamous2-recomp) |
| `patches/port/`, `port_src/` | As mudanças deste projeto sobre o `base/` |
| `natives/` | Os jobs SPU reescritos em C |
| `tools/` | O Setup, o build e as ferramentas de teste |

Tudo é MIT. O `ppu_nv_dirty.py` veio de `soyjxck/ps3recomp`, também MIT.

---

## Parte 2: onde está o gargalo do Battride

Os números abaixo saem do `docs/PERFORMANCE.md` do Battride.

- **O limite é a thread `runTaskMain`**, a thread PPU principal do jogo, entre 85% e 93% de um núcleo. O
  `frame_clock` (walker da FIFO mais draw engine) fica em cerca de 55%, o áudio SPU em cerca de 45% e a GPU não é o
  limite.
- **Na `runTaskMain`:**
  - `func_004B5120`, uma busca numa lista: cerca de 10%, limitada por latência (em média 250 nós por chamada, 20–25
    mil chamadas por segundo, todas acham o nó);
  - os helpers de memória do guest: de 12% a 20%, espalhados por centenas de chamadores. O custo está nos checks,
    não na chamada;
  - `func_005B2930`, a montagem da lista de comandos (chunk 020), que cresce de 5,6 para 10,5 ms nos frames pesados;
  - `func_005C79B4` e os helpers VMX.
- **O jogo se limita a 60 fps** pelo `func_003ABE20`, que dorme milissegundos inteiros e depois gira no timebase até
  o prazo do frame. Por isso as médias de builds diferentes encostam nos 60: por exemplo, 59,4 contra 59,1 fps no
  store gate.
- **O lifter do SDK fixado só tem** `PPU_FENCE`, `PPU_NO_INLINE_LOADS` e `PPU_THREAD_LOCAL`. Cada instrução lê e
  grava `ctx->gpr[]`, `ctx->cr` e `ctx->xer` na memória.
- **No SDK, `vm_base` é uma variável global** (`extern uint8_t* vm_base`), e o executável é PIE.

---

## Parte 3: regras de medição e adoção

Valem para todas as fases.

- **Benchmark:** `scripts/perf/bench_combat.py` na STG02, 60 s de luta, **3 pares intercalados A/B**.
- **Métrica principal:** o `busy_ms` da fase F1, que é o tempo do frame sem a espera do limitador, mais os piores 5
  s e o p95. Nas fases gráficas, use o fps sem limite (F2) ou o tempo de GPU.
- **Regra de adoção:** a mudança entra se melhorar a média e os piores 5 s sem piorar o p95.
- **Verificação:** a build `*_VERIFY` ou `=check` precisa rodar limpa no boot, nos menus e no combate.
- **PGO:** retreinar ao fim de cada fase adotada que mude o codegen, não a cada switch.
- **Fases gráficas:**
  - cada recurso tem um switch, e com tudo desligado a imagem é exatamente a de hoje;
  - comparar `PORT_WINDOW_DUMP` ligado e desligado no mesmo frame, contra as filmagens do jogo original;
  - medir o custo na **Radeon integrada**: o padrão não pode tirar a STG02 dos 60 fps nela;
  - expor no launcher GTK (página Settings) com uma linha dizendo o custo e se precisa reiniciar.

---

## Parte 4: fase F, desbloqueio de fps e métrica de folga

Esta fase vem logo depois da fase 0, porque é ela que mede todas as outras.

**Diferença entre os dois jogos.** O inFAMOUS 2 tem **simulação de passo variável** (o dt é limitado entre 0 e 1/30
s em `func_00089610`), então lá basta um cap no present. O Battride **se limita sozinho** (`func_003ABE20`) e o
vblank é sintetizado a cerca de 60 Hz no `main.cpp`. Os logos duram o mesmo a 20 e a 60 fps, mas isso **não prova**
que o combate usa dt.

### F1: métrica de folga, sem mudar o jogo (cerca de 1 dia)

1. Um wrapper de `func_003ABE20` com o mesmo mecanismo do `ppu_native.cpp` (símbolo fraco). Ele chama o lifted e
   mede quanto tempo ficou dentro.
2. Calcular por frame: `busy_ms = intervalo − espera no limitador`, e daí `fps potencial = 1000 / busy_ms`.
3. Gravar como colunas novas no `PORT_FRAME_TIMES` e mostrar no `frame_report.py` e no `bench_combat.py` (média, p95
   e p99 do `busy_ms`, mais o fps potencial).
4. Validar com `PORT_FPS_LIMIT=20` ou `30`: o `busy_ms` precisa continuar parecido, e a espera vai a cerca de 0
   abaixo de 60.

### F2: uncap para benchmark (1–2 dias)

5. Com `PORT_UNCAP=1`, o wrapper pula a espera e retorna com o mesmo estado do lifted. Comparar num modo `=check`.
6. Se o jogo esperar pelo vblank sintetizado (ver no `PS3_FTRACE`), criar `PORT_VBLANK_HZ` ou passar o flip para o
   modo imediato. Um cap com ritmo uniforme (timer de alta resolução mais spin) segura melhor 120 e 144.
7. Usar o present em `IMMEDIATE` ou `MAILBOX`, para o vsync não virar o novo teto.
8. **Teste de velocidade:** medir na mesma cena, a 60 e sem limite, a animação de um ataque especial, a distância de
   um dash, o timer da missão e uma cutscene em engine.
   - **Mesma duração:** o jogo usa dt. Siga para F3.
   - **Mais rápido sem limite:** o jogo é de passo fixo. O uncap serve só como teste de estresse, e a F1 continua sendo
     a métrica oficial.
9. A rota do `bench_combat.py` é guiada por tempo, então só use F2 no benchmark se o item 8 der "mesma duração".

### F3: 120/144 fps para o jogador (opcional, depende do item 8)

- **Se o jogo usa dt:**
  - cap no launcher (60, 120, 144, sem limite), com 60 como padrão;
  - validar uma sessão inteira da STG02 em 120 e 144: física, hitboxes, IA, câmera e áudio;
  - conferir se o dt tem um limite máximo;
  - manter vídeos e cutscenes no ritmo original.
- **Se o jogo é de passo fixo:**
  - patch na constante de dt (provavelmente um 1/60 em float), com risco em tudo que conta frames (scripts, hitstun,
    janelas de input);
  - interpolação da renderização entre passos, que **não é recomendada** numa recompilação.

  Se nenhuma for viável, a F3 cai e o desbloqueio fica só como ferramenta de medição.

---

## Parte 5: fases 0 a 5, otimizações do lifter vindas do infamous2

O lifter do infamous2 (`engine/tools/ppu_lifter.py`, `ppu_fastpath.py`, `ppu_nv_dirty.py` e `ppu_vmx_flow.py`)
resolve no gerador de código, para todas as funções, o que o Battride fez à mão função a função (`ppu_native.cpp`,
`ppu_vmx.h`, `store_wrap.c`, store gate). No infamous2, a fase 1 sozinha cortou **26% do tempo por frame** no código
PPU da thread principal.

### Fase 0: preparação (cerca de 1 dia)

1. Vendorizar os quatro arquivos em `tools/lifter/`, com os avisos MIT, sem mexer no SDK fixado.
2. Adicionar ao `scripts/build/generate.py` a opção `--lifter vendored|sdk` e um dicionário de switches passado por
   variável de ambiente.
3. **Prova:** o lifter novo, com todos os switches desligados, precisa compilar com os bridges e rodar igual ao
   atual. Aqui aparecem as diferenças de interface com o `a6d76c8`, como o `ppu_recomp.h` ou o `g_trampoline_fn`.
4. Guardar esse build como baseline. Conferir que o símbolo fraco de `func_004B5120` e o `-include` nos chunks
   015/017/021/029/030 ainda batem (`--chunk-lines 80000`).

### Fase 1: registradores locais, o maior ganho esperado (1–2 dias)

5. `PPU_NV_LOCALS` e `PPU_MM`, primeiro com `PPU_NV_VERIFY=1`.
6. Somar `PPU_DRAIN_ELIDE`.
7. `PPU_SO_ZERO` só depois de provar que o EBOOT nunca liga XER[SO]: procurar `mtxer`, `addo.`, `subfo.` e as outras
   instruções com o sufixo `o`.
8. Medir os fixes manuais em três variantes: com o `native_004B5120` e o `port_ppu_memory.h`, sem os dois, e só com o
   native.

   O `port_ppu_memory.h` pode conflitar com os gates do `PPU_MM`. Se conflitar, use
   `-DPORT_PPU_FAST_MEMORY=OFF`.

### Fase 2: VMX e ponto flutuante (1–2 dias)

9. Portar o `runtime/ppu/ppu_vmx_sse.h`.
10. `PPU_VMX_SSE` e `PPU_FP_ONE_NAN`, depois `PPU_VMX_FAST`. Rodar `test_vmx_sse.c` e `test_fp_fast.py` junto com o
    `test_vmx_simd.sh`.
11. `PPU_VMX_CLEAN`, `PPU_VMX_INT`, `PPU_FP_SINGLE` e `PPU_FP_CVT`, primeiro com `*_VERIFY=1`.
12. Aposentar as versões SSE do `include/ppu_vmx.h` que o `ppu_vmx_sse.h` passar a cobrir.

### Fase 3: stores e loads mais leves (2–4 dias)

13. `PPU_ST_LEAN`, depois `PPU_ST_PMAP`. O `PPU_ST_PMAP` precisa do `g_ppu_st_pmap` no runtime, atualizado junto com
    o bitmap de reserva (`spu_coherency`, `spu_raw`). Escolha **um** caminho de store só, entre ele e o
    `store_wrap.c` com o `port_ppu_store_end`.
14. `PPU_MOVBE`, que só funciona em CPUs com MOVBE (Haswell ou mais novo, Jaguar ou mais novo). Gerar só quando a CPU
    suportar e documentar as que ficam sem (Penryn, Nehalem).
15. `PPU_TOC_FOLD`, que precisa do `g_ppu_toc_ok` e do `g_ppu_toc_dirty`.
16. `PPU_LEAN_MEM` por último, porque é o mais invasivo:
    - portar `runtime/memory/vm_guard.c` e `vm_guard.h`, que trocam o teste de faixa por guard pages;
    - instalar o handler de falha (SIGSEGV no Linux, VEH no Windows) e registrar `ppu_lean_mem_register`;
    - conferir o poll da palavra de referência no `gcm_bridge.c` com o `test_gcm_jump.sh`.

### Fase 4: chamadas (cerca de 1 dia)

17. `PPU_XINLINE` (com o `PPU_XINLINE_MAX` padrão de 48) e `PPU_ICALL_INLINE`. Acompanhar o tamanho do executável.
18. `PPU_XTAIL` e `PPU_DRAIN_NONE` exigem `musttail`, ou seja, clang. Com GCC, ficam desligados.

### Fase 5: clang + ThinLTO (opcional, 3–5 dias)

19. Um preset `linux-vulkan-clang` com `-O2 -march=native -flto=thin`, link com lld e o cache do ThinLTO.
20. Migrar o PGO: `-fprofile-generate`, depois `llvm-profdata merge`, depois `-fprofile-use`. Adaptar o
    `pgo_train.py` e o `pgo_relpaths.py` (`-ffile-prefix-map`).
21. Medir GCC+PGO contra clang+ThinLTO+PGO. Só se o clang ganhar: ligar `PPU_XTAIL` e `PPU_DRAIN_NONE` e trocar o
    toolchain do AppImage e do pacote Windows (no MSYS2, o ambiente CLANG64).

**Extra:** o `rsx_split.c` do infamous2 divide o walker da FIFO e o draw engine em duas threads. Vale só se o walker
virar o gargalo depois das fases 1 a 3 (veja M9).

---

## Parte 6: melhorias próprias do Battride (M1 a M10)

Estas ideias não dependem do port do infamous2. Saem do profile do Battride e do SDK fixado.

### M1: cache validado para `func_004B5120` (até ~10% da thread) [precisa do EBOOT]

A busca é limitada por latência, então o caminho é **não andar pela lista**. Em vez de invalidar o cache em toda
função que mexe na lista, **valide o acerto em O(1)**:

- O cache guarda `(cabeça da lista, chave) → nó`.
- Num acerto, ele confere que:
  - `nó+0xC` ainda tem a chave;
  - o nó ainda está na lista, se ele tiver ponteiro `prev` ou de dono;
  - não há nó anterior com a mesma chave. Isso só vale se as chaves forem únicas, o que dá para provar com
    `PORT_PPU_NATIVE=check` em várias sessões.
- Se qualquer verificação falhar, ele anda pela lista como hoje e atualiza o cache.

Se o nó não tiver como provar que pertence à lista, instrumente só as funções de inserção e remoção daquela lista,
com um contador de geração.

### M2: substituições nativas escolhidas pelo profile (5–15%, aos poucos) [precisa do EBOOT]

- Um script que lista o top 20 do `runTaskMain` por tempo próprio e inclusivo.
- Candidatos: `func_005B2930`, `func_005C79B4`, `func_005AAC60` e `func_003C7A98`.
- **A técnica principal no `func_005B2930`:** ele escreve palavras em sequência no command buffer da GCM. A versão
  nativa pode checar o limite e a reserva **uma vez por bloco contíguo**, em vez de uma vez por store.
- Cada função entra só com o `=check` limpo e três pares intercalados.

### M3: a base da memória do guest como constante (2–5%, barato)

Em cada acesso, o endereço do `vm_base` sai do GOT e o GCC não consegue manter o valor num registrador depois de
stores por `uint8_t*` ou de chamadas. Três experimentos, do mais barato ao mais caro:

1. `__attribute__((visibility("hidden")))` no `vm_base`, no `g_trampoline_fn` e no `port_ppu_store_end`.
2. Compilar com `-fno-pie -no-pie`.
3. Mapear a arena num endereço fixo (`MAP_FIXED_NOREPLACE` ou `VirtualAlloc` com endereço), com fallback, e definir
   `vm_base` como constante. Assim um acesso vira `movbe eax, [ea + CONST]`.

Os três mudam todas as funções, então o PGO precisa ser retreinado.

### M4: registradores em locais, no lifter (15–25%, a maior e mais cara)

Generalizar no `ppu_lifter.py` o que foi feito à mão no `native_004B5120`: registradores em variáveis locais,
gravados de volta só antes de chamadas e retornos. Junto com isso, estender a eliminação de campos do CR que nunca
são lidos (parcial no SDK, por volta da linha 750) para o XER[CA] e para o CR entre blocos. Uma build de verificação
compara o `ctx` no retorno de cada função.

**É o mesmo objetivo da fase 1.** Faça uma das duas: portar o `PPU_NV_LOCALS` (mais rápido) ou escrever a própria
passagem.

### M5: timebase via `rdtsc` (0%, ou 10%+ com HPET)

Com HPET como fonte de relógio, o `ppu_timebase_now` custou 11–13% do `runTaskMain`. Quando a CPU tem `constant_tsc`
e `nonstop_tsc`, leia o `rdtsc` direto e escale para os 79,8 MHz do timebase, calibrando uma vez no início. Sem TSC
confiável, fica o caminho atual.

### M6: tabela plana para chamadas indiretas (0–3%) [medir antes]

Se o `ps3_indirect_call` passar de 1% do `runTaskMain`, troque a busca por uma tabela indexada por
`(alvo − início_do_texto) >> 2`, que devolve o ponteiro da função lifted ou `NULL`.

### M7: BOLT depois do link (2–8%)

- O `llvm-bolt` reordena funções e blocos do binário já linkado. Funciona com GCC (`-Wl,--emit-relocs`).
- O Zen 2 não tem LBR, então use o modo `-instrument`.
- A build do jogador usa `-march=native`, então o profile não casa direto. Teste primeiro só no seu ambiente.
- Como os huge pages renderam pouco (+0,9 fps), espere um ganho moderado.

### M8: tirar trabalho do host da thread principal [precisa rodar o jogo]

Separe no sampler as amostras do `runTaskMain` que caem **fora** do código lifted: callbacks da GCM quando o command
buffer enche, `sys_lwmutex`, `sys_lwcond`, leituras de arquivo. Tudo o que for síncrono e puder ser assíncrono é
ganho direto.

### M9: saber se o walker entra no caminho crítico [precisa rodar o jogo]

Meça quanto o `runTaskMain` espera pelo fence em cada frame.

- **Perto de 0 nos frames pesados:** o walker não importa.
- **Se crescer:** reduza no walker o vertex fetch (22%), a decodificação da FIFO (21%) e os hashes de textura (11%),
  ou divida o walk e o draw engine em duas threads.

### M10: CPUs pequenas

- Garantir que o `thread_affinity.c` nunca ponha o áudio no núcleo do `runTaskMain`.
- Dar prioridade menor ao áudio quando houver menos de 6 núcleos físicos.
- Medir limitando a afinidade do processo no 4600H.

---

## Parte 7: fases G1 a G7, melhorias gráficas

O `vulkan_bridge.c` do Battride é uma cópia local do backend Vulkan do SDK. O infamous2 levou o mesmo backend bem
mais longe (`engine/libs/video/`), então o trabalho é trazer módulos para os bridges, não trocar o backend inteiro.

**O que o Battride já tem:** escala interna até 4×, FSR 1 com nitidez e 16:9 com barras pretas.

### G1: filtro anisotrópico (horas)

- **Origem:** `vk_postfx.c`, opção `PS3RECOMP_ANISO=1..16`, padrão 16.
- **O que fazer:** aplicar `maxAnisotropy` nos samplers com mip e filtro linear. Point, comparação (shadow maps) e UI
  ficam como estão.
- **Pré-requisito:** a feature `samplerAnisotropy`. Sem ela, 1×.

### G2: AA pós-processado e nitidez (1–2 dias)

- **Origem:** `vk_postfx.c`/`.h`, com `PS3RECOMP_AA=off|fxaa|smaa` e `PS3RECOMP_SHARPEN` (CAS/RCAS).
- **Ordem do pipeline:** SMAA na resolução interna, depois o FSR 1 (que já tem RCAS: não aplique nitidez duas
  vezes), depois o present.
- **O trabalho principal é tirar o HUD do AA.** Achar o último draw da cena antes do HUD com o `PORT_SURFACE_LOG` e o
  hash do fragment program (o memo do `rsx_engine_bridge.c`).
- **Licença:** as texturas de área e de busca do SMAA entram em `third_party/`, MIT, no `THIRD_PARTY_NOTICES.md`.

### G3: MSAA e "thin lines" (2–3 dias)

- **Origem:** `vk_msaa.inc` e `PS3RECOMP_ALPHA_AA`.
- **Quais alvos:** só os do tamanho da tela do guest. Shadow maps, efeitos e glow ficam em 1×.
- **Pontos difíceis:**
  - resolver o multisample antes de cada leitura;
  - o `PORT_DEPTH_SNAPSHOT*` vai precisar de resolve de profundidade;
  - o `PORT_VK_MERGE_PASSES` precisa saber dos alvos multisample.
- **Alpha to coverage** para cercas, fios e folhagem. O teste de alpha escalado pelo mip vale mesmo sem MSAA.
- **Custo:** no infamous2, o 4× tirou cerca de 40% do fps em 1080p. Deixe desligado por padrão.

### G4: sombras em resolução maior (2–4 dias) [investigação]

- **Origem:** `rsx_shadow_res.inc` (`PS3RECOMP_SHADOW_RES`, 2× ou 4×).
- **Mecanismo genérico:** reconhecer o passe de sombra (só profundidade, num zeta quadrado), criá-lo maior e
  reescalar viewport e scissor.
- **Específico do jogo:** o endereço do shadow map, os programas que o amostram e as constantes de offset de texel
  (do tipo 1/1024) no fragment program.
- **Passo 1:** achar o mapa das sombras dos personagens com o `PORT_SURFACE_LOG` numa luta da STG02.
- **Atenção:** o `PORT_SHADOW_REVERSE` já mexe nesse caminho.

### G5: efeitos em resolução cheia (1–3 dias, depende do jogo)

- **Origem:** o bloco `eng_fxres` de `rsx_draw_engine.c` (`PS3RECOMP_FX_RES_TARGETS`).
- **Só vale se o Battride desenhar partículas ou glow num alvo reduzido.** Os primeiros candidatos são os efeitos
  dos ataques especiais.
- **Passo 1:** procurar no surface log alvos de 640×360, 512×288 ou parecidos. Se não houver, a fase cai.

### G6: widescreen real, Hor+ (5+ dias, a mais cara)

- **Origem:** `engine/docs/WIDESCREEN.md`, com duas metades.
  - **Backend (genérica):** alvos e janela no formato da tela, com o HUD composto depois.
  - **Jogo (específica):** um patch no EBOOT que alarga o FOV horizontal das câmeras.
- **Caminho no Battride:**
  - procurar no `patch.yml` do RPCS3 um patch de widescreen para o BLJS10220;
  - se não houver, achar a função que monta a projeção.
- **Riscos:** HUD esticado, culling que corta inimigos nas bordas, cutscenes enquadradas para 16:9. As barras pretas
  continuam como padrão.
- **Ordem:** depois da fase 0, porque o `PPU_PATCH_FILE` vem com o lifter novo.

### G7: pacotes de texturas (1–2 dias)

- **Origem:** `PS3_MODS` e `PS3_MODS_DUMP` em `rsx_draw_engine.c`, mais `rsx_image_io.c` (stb_image).
- **Como funciona:** o dump grava cada textura como PNG pelo hash do conteúdo, e uma pasta de mods substitui a
  textura no upload. O gancho existe, porque o Battride já faz hash de textura no cache.
- **Valor:** é uma porta aberta para a comunidade, não um ganho visual por si só.

---

## Parte 8: o que não fazer

**Já medido e rejeitado no `PERFORMANCE.md` do Battride:**

- `-O3` nos chunks 015 e 021;
- inline de memória em mais chunks ou por função;
- huge pages na arena do guest;
- GameMode;
- timer slack de 1 µs;
- encurtar o intervalo de publicação do fence;
- prefetch na busca da lista.

**Não se aplica ao Battride:**

- os `natives/` do infamous2, que são jobs SPU do Edge específicos do inFAMOUS 2. A SPU de áudio não é o limite;
- SSAO e iluminação "na GPU" (`gpu_ssao.c`, `gpu_light.c`): no inFAMOUS 2 eram jobs SPU próprios, e o Battride não
  tem equivalente documentado;
- o menu F2 dentro do jogo: o launcher GTK já cumpre esse papel. A comparação PS3/seu modo com F3 é um bônus depois
  de G1 a G3.

---

## Parte 9: instalação no Windows (W1 a W4)

**Como está na v0.6.0** (`docs/WINDOWS_PACKAGE.md`):

- O pacote é um zip de 742 MB com Python, GTK 4 e um subconjunto do MSYS2 UCRT64. O jogador extrai à mão para uma
  pasta curta e abre `BattrideWarRecomp.cmd`.
- O pacote aninha arquivos a 124 caracteres de profundidade, então a pasta dele pode ter no máximo 130 caracteres
  (`WINDOWS_PACKAGE_PATH_MAX`). O build aninha cerca de 155 abaixo da pasta de dados, que pode ter no máximo 100
  (`WINDOWS_DATA_PATH_MAX`). O launcher só avisa depois que o zip já foi extraído.
- Nada é assinado:
  - o SmartScreen avisa sobre o conteúdo do zip;
  - o Smart App Control do Windows 11 já bloqueou uma vez o `game-runner.exe` recém-compilado;
  - o Defender escaneia cada arquivo do build e deixa a compilação mais lenta.
- Só o Windows 11 foi testado de ponta a ponta, numa única máquina.

O instalador Inno Setup do `docs/ROADMAP.md` (planejado para a 0.6.1) resolve parte disso: pasta padrão curta e
checagem do caminho antes de copiar. Os itens abaixo complementam o instalador ou atacam a causa.

### W1: o limite de 260 caracteres

Do mais barato ao mais completo:

1. **Medir de onde vem a profundidade.** Um script sobre o stage do pacote e sobre uma árvore de build lista os 50
   caminhos mais longos, relativos à raiz. Os suspeitos comuns são:
   - `usr/share/icons/Adwaita/...`, locales e docs do MSYS2;
   - `site-packages` do Python e `__pycache__`;
   - testes e `third_party` do SDK;
   - os `CMakeFiles/<alvo>.dir/<caminho da fonte>.obj` do CMake.
2. **Cortar o que é fundo e não é necessário.** No `build_windows_package.py`: ícones simbólicos e escalonáveis que
   o launcher não usa, locales fora de en/ja/pt, documentação, testes e `__pycache__` do MSYS2 e do SDK. Cada
   diretório removido baixa o limite de 124 e encolhe o zip. Valide abrindo o launcher e fazendo um build completo.
3. **Encurtar os caminhos do build.**
   - Nomes curtos para os diretórios de trabalho dentro da pasta de dados (`b/`, `s/`, em vez de nomes descritivos).
   - `CMAKE_OBJECT_PATH_MAX` e, nos chunks gerados, nomes curtos de alvo e de fonte.
   - O objetivo é que os 155 caracteres abaixo da pasta de dados caiam para menos de 100. Com isso, um usuário com
     nome longo em `C:\Users\...` deixa de ser problema.
4. **Junction automática quando o caminho continuar longo.** O launcher cria, sem precisar de administrador
   (`mklink /J`), um atalho curto como `%LOCALAPPDATA%\bwr\p` para a pasta do pacote e `...\bwr\d` para a de dados,
   e roda o importer e o build por ele. Os arquivos continuam onde estão.
   - **Atenção:** o `packaging/importer.py` e o `launcher.py` usam `Path.resolve()`, que no Windows segue junctions
     e devolveria o caminho longo. Nesses pontos, use `Path.absolute()` no Windows.
   - O CMake e o GCC também podem resolver o caminho. Teste um build inteiro com o pacote numa pasta de 200
     caracteres.
5. **Não dá para depender** do `LongPathsEnabled` do registro: precisa de administrador, e o GCC do MSYS2 não declara
   `longPathAware` no manifesto.

**Junto com isso, contra uso errado:**

- **Rodar de dentro do zip.** O `.cmd` verifica se `usr\bin\python.exe` existe ao lado dele. Se não existir, ou se o
  caminho contiver `\Temp\` com `.zip`, ele mostra uma caixa de mensagem ("extraia o zip primeiro") em vez de falhar
  em silêncio.
- **A extração do próprio Explorer também para em 260 caracteres**, às vezes com um erro genérico. Enquanto não
  houver instalador, a página de release deve recomendar o 7-Zip ou dizer qual mensagem de erro esperar. Com o
  instalador, o problema some.

### W2: os avisos por falta de assinatura

1. **Assinar o que é distribuído:** o instalador, o executável do launcher e os `.exe` e `.dll` próprios do pacote.
   Opções a comparar (confira preços e se aceitam desenvolvedor individual no Brasil, porque isso muda):
   - **SignPath Foundation:** gratuito para projetos open source, assina a partir de CI (GitHub Actions). Exige que
     o build do pacote rode no CI.
   - **Azure Trusted Signing:** assinatura mensal barata, com validação de identidade.
   - **Certificado Open Source da Certum:** anual, de baixo custo, para desenvolvedor individual.

   Mesmo com assinatura, um certificado novo começa sem reputação no SmartScreen. Os avisos diminuem conforme os
   downloads se acumulam, e o certificado precisa ser o mesmo em todas as versões.
2. **Trocar o `.cmd` por um executável pequeno**, que o roadmap já prevê. Um `.cmd` baixado da internet sempre
   provoca o aviso, e um `.exe` assinado acumula reputação. De quebra, some o risco de flash de console.
3. **O jogo compilado na máquina do jogador não pode ser assinado com o seu certificado**, porque a chave privada não
   pode ir no pacote. Por isso o bloqueio do Smart App Control pode voltar. Há duas saídas:
   - **Testar** se o Smart App Control aceita um `.exe` hospedeiro **assinado e distribuído por você** que carrega o
     código do jogo compilado localmente como DLL (o mesmo desenho do infamous2: engine pronta mais
     `Infamous2Game.dll`). Não sei se o Smart App Control libera DLLs não assinadas nesse caso, então isso precisa ser
     testado numa máquina com ele em modo de bloqueio antes de mudar o build.
   - **Documentar** como hoje, se o teste falhar.
4. **Falsos positivos do Defender:** envie o pacote e uma build de exemplo do `game-runner.exe` pelo formulário de
   envio de arquivos da Microsoft, a cada versão.
5. **Lentidão do build com o Defender.**
   - Medir o tempo do build com e sem a exclusão da pasta de dados.
   - Documentar o **Dev Drive** do Windows 11 (ReFS com antivírus em modo de desempenho) como opção para quem quiser.
   - Não pedir exclusão por padrão: precisa de administrador e reduz a segurança do usuário.
   - Menos arquivos intermediários também ajudam, por exemplo agrupar os chunks pequenos.

### W3: o Windows 10 sem teste

1. **Matriz mínima de teste:**

   | Sistema | O que cobre |
   |---|---|
   | Windows 10 22H2, instalação limpa | o caso principal |
   | Windows 10 LTSC 2021 | sistemas enxutos, sem a Store |
   | Windows 11 24H2 | o que já funciona, como referência |

2. **Em máquina virtual** dá para testar: extração, launcher (GTK 4), download do RPCS3, importação do disco, build
   completo e a tela da checagem gráfica.
   - O GTK 4 pode falhar sem GPU na VM. Teste `GSK_RENDERER=cairo` como alternativa e, se funcionar, deixe o launcher
     cair nele sozinho.
   - O jogo em si precisa de Vulkan, então precisa de hardware real ou passthrough de GPU.
3. **Casos de caminho que valem nos dois Windows:**
   - nome de usuário com acento ou com espaço (`C:\Users\João Silva`);
   - nome de usuário longo, perto do limite de 100 da pasta de dados;
   - pasta Documentos ou Área de Trabalho redirecionada para o OneDrive;
   - pacote numa unidade diferente de `C:`.
4. **Pedir testes à comunidade.** Um modelo de issue "Teste no Windows" pede a versão do Windows, a CPU, a GPU com
   driver, o caminho do pacote e da pasta de dados (com o nome de usuário ocultado) e os quatro logs (`import.log`,
   `build.log`, `graphics.log`, `game.log`).

### W4: validar a 0.6.1

Antes de remover "Windows 10 needs testing" das notas de versão:

- instalação limpa com o instalador nos três sistemas da matriz, sem administrador;
- o caminho de 200 caracteres (W1, item 4) até a tela de título;
- o zip aberto sem extrair, mostrando a mensagem certa;
- uma atualização sobre a 0.6.0, com os saves mantidos;
- SmartScreen e Smart App Control com o pacote baixado pelo navegador, anotando o que cada sistema mostra.

---

## Parte 10: atualizações sem recompilar tudo (U1 a U6)

### Por que o jogador sempre compila uma vez

**A primeira compilação não pode sumir, e o motivo é legal, não técnico.** O jogo compilado é o código do EBOOT
traduzido para C e depois para código de máquina: uma obra derivada do jogo. A 0.2.0 trazia esse executável pronto e
por isso não pôde ser publicada (`docs/APPIMAGE.md`, "Builds on the player's computer in 0.3.0"). Compilar na máquina
do jogador, a partir do disco dele, é o que permite publicar o projeto.

As alternativas esbarram no mesmo ponto:

- **Patch binário** (xdelta) do EBOOT para o executável: os dois quase não têm bytes em comum, então o patch conteria
  o jogo traduzido inteiro. É distribuir o código do mesmo jeito.
- **Compilar num servidor** e devolver ao jogador: o projeto passaria a receber EBOOTs e a distribuir o jogo
  compilado, além de pagar o servidor. Descartado.
- **A engine pronta (S3)** diminui o pacote, mas quase não muda o tempo: os 84 arquivos PPU traduzidos são a maior
  parte da compilação, e eles continuam sendo compilados pelo jogador.

**O objetivo da trilha U é outro: compilar uma vez só, e mais rápido.** Na instalação o jogador compila (12 min com
4 jobs no Linux e cerca de 40 min com 2 jobs no Windows, no Ryzen 5 4600H, medido em 17 e 18/11), e depois quase
nunca mais. Por isso a trilha U entra **na 0.6.1**, junto com a do Windows, e não depois.

**Exemplo real.** O `90a666c` só mudou documentação, mas ela vai dentro dos pacotes e entra no hash; quem atualizar da
0.6.0 para uma versão com esse commit vai recompilar tudo. Depois da U1, uma mudança assim não recompila nada.

### Como está na v0.6.0 (`packaging/importer.py`)

O jogo compilado só é reaproveitado se o `runner/build.json` for **idêntico** ao `build_identity()`. Esse valor junta:

- `BUILD_RECIPE` (hoje, 3);
- **a versão do pacote**;
- o commit do SDK;
- o SHA-256 do EBOOT;
- **o hash de todos os arquivos em `usr/share/battride/port`**;
- a identidade da CPU;
- se existe PGO.

Se qualquer um deles mudar, o `build_runner()` faz quatro coisas:

1. cria uma pasta de trabalho nova, com nome aleatório (`scratch_name('.b-')`);
2. copia a árvore do port inteira;
3. roda o `generate.py`, que traduz de novo os 84 arquivos PPU e os programas SPU;
4. configura o CMake e compila tudo.

No fim, `finally: shutil.rmtree(work)` apaga a pasta de build. Não há build incremental nem ccache.

**O hash cobre mais do que o binário usa.** A árvore copiada é **todo** o `PUBLIC_FILES.txt` (`build_windows_package.py`
linha 84, e o mesmo no `build_appimage.py`). Isso inclui `README.md`, `CREDITS.md`, `THIRD_PARTY_NOTICES.md`, todos
os `docs/*.md`, `tests/*`, `scripts/perf/*`, `scripts/release/*`, `packaging/launcher.py` e
`packaging/importer.py`. Corrigir um erro de digitação na documentação, ou só subir a versão, faz o jogador
recompilar por 15 a 60 minutos.

### U1: identidade só com o que afeta o binário (horas)

> **Feito em 19/11** (`packaging/importer.py`, `BUILD_RECIPE` 4). A identidade passou a ter `sources` (só os
> `BUILD_INPUTS`: `CMakeLists.txt`, `recompile.json`, `app.rc`, `include/`, `port/`, `shaders/`, `third_party/`,
> `scripts/build/`, `patches/ps3recomp/` e `packaging/linux/glslang-compat/`), `toolchain` (hash do
> `app/build-packages.txt` no Linux ou do `usr/toolchain/packages.json` no Windows), `game_libs` e `pgo` (hash dos
> perfis, não só se existem); a versão do pacote saiu. A lista fica no `importer.py`, que os dois pacotes usam, em vez
> de uma marca no `PUBLIC_FILES.txt`. Ao revisar apareceram dois buracos que a versão escondia: perfis retreinados e um
> toolchain novo não recompilariam o jogo; agora recompilam. Testes novos no `tests/test_importer.py`: docs, testes,
> launcher, importer e versão não mudam a identidade; `port/`, `include/`, `CMakeLists.txt`, `generate.py`, perfis e
> toolchain mudam; e todo caminho que o `CMakeLists.txt`, o `generate.py`, o `lift_audio_spu.py` e o
> `build_runner()` leem está nos `BUILD_INPUTS` (conferido tirando `include/` e `glslang-compat/` da lista: o teste
> falha nos dois). Falta a validação com pacotes reais (U5).

1. Trocar o `sources_sha256()` por uma lista explícita do que o CMake e o `generate.py` leem: `CMakeLists.txt`,
   `CMakePresets.json`, `recompile.json`, `config.toml`, `port/`, `include/`, `shaders/`, `third_party/`,
   `scripts/build/` e `patches/ps3recomp/`.
   - A lista fica num lugar só (por exemplo, uma marca `build:` no `PUBLIC_FILES.txt`), para não divergir entre o
     AppImage e o pacote Windows.
   - Um teste no `tests/test_publication.py` confirma que todo arquivo citado pelo `CMakeLists.txt` e pelo
     `generate.py` está na lista.
2. Tirar `package` (a versão) do `build_identity()`. Subir o `BUILD_RECIPE` quando a receita do build mudar.
3. Testes no `tests/test_importer.py`:
   - mudar um `.md`, um teste ou o `launcher.py` **não** muda a identidade;
   - mudar um arquivo de `port/` **muda**.
4. Na primeira atualização com essa regra, a identidade nova não vai bater com o `build.json` antigo, então haverá
   **um** rebuild. A nota de versão precisa dizer isso.

### U2: reaproveitar a tradução (cerca de 1 dia)

> **Feito em 19/11.** `scripts/build/generate.py --cache <pasta>` guarda uma cópia de `src/recomp/`, `spu/` e
> `analysis/regeneration/` em `<pasta>/gen-<chave>/` e a reaproveita quando a chave bate. A chave junta o EBOOT, as
> ferramentas do SDK (`tools/`), os patches do SDK, o `generate.py`, o `lift_audio_spu.py`, o `recompile.json`, os
> codecs do firmware e as versões do Python e do Capstone; o `--jobs` fica de fora. O importer passa
> `--cache <dados>/cache`. Fica só a entrada atual; as antigas são apagadas. Na reutilização, o único arquivo com o
> caminho da árvore (`spu/sources.cmake`) é reescrito para a pasta nova. Medido neste notebook: a tradução levou 86 s
> e a reutilização menos de 1 s; os arquivos saíram idênticos entre as duas, e iguais aos que o repositório gerou. O
> cache ocupa cerca de 600 MB na pasta de dados. Testes em `tests/test_generate.py`. Na prática a opção ficou no
> `generate.py` com o nome `--cache`, em vez do `--reuse` descrito abaixo.

5. Guardar o `src/recomp/`, o `spu/` e o `analysis/regeneration/` gerados em `<dados>/cache/gen-<chave>/`. A chave é
   o hash do EBOOT, do firmware usado no áudio, do commit do SDK, dos patches do SDK, do `generate.py`, do
   `lift_audio_spu.py` e das opções de geração.
6. Uma opção `--reuse <pasta>` no `generate.py`, que aceita sources que já existem só quando a chave gravada bate.
   Hoje ele se recusa a substituir `src/recomp/`, `spu/` e `analysis/regeneration/`.
7. Manter só a chave atual e apagar as antigas. Com isso, a etapa "Step 1 of 2: translating the game code" some das
   atualizações que não mexem na tradução.

### U3: compilação incremental com ccache (1–2 dias)

8. Distribuir o ccache no pacote Windows (há um pacote para o MSYS2 UCRT64) e no toolchain do AppImage. Ligar com
   `CMAKE_C_COMPILER_LAUNCHER` e `CMAKE_CXX_COMPILER_LAUNCHER`.
   - No PGO, o `pgo_relpaths.py` já é launcher. Encadeie os dois (ccache chamando o `pgo_relpaths.py`, ou o contrário)
     e confira que a reescrita de caminhos continua valendo.
9. **Pasta de trabalho com nome fixo** (`<dados>/.b/`) em vez de aleatório, mais `CCACHE_BASEDIR=<dados>`, para os
   caminhos baterem entre builds.
10. **Separar o cache pela CPU.** Como o build usa `-march=native`, o ccache vê a mesma linha de comando em CPUs
    diferentes. Use o `namespace` do ccache (4.8+) ou um diretório por hash do `cpu_identity()`. Sem isso, quem mover
    a pasta de dados para outro PC pode receber objetos compilados para a CPU errada.
11. Limitar o tamanho (`max_size`, por exemplo 3 GB). O launcher mostra o espaço ocupado e tem um botão "Limpar cache
    do build". O cache não entra na checagem de 4 GB livres antes do build.
12. **A identidade continua decidindo se recompila.** O ccache só faz a recompilação ser rápida. Se o cache for
    apagado, o build volta a ser completo e o resultado é o mesmo.

### U4: política dos perfis de PGO

> **Script feito em 19/11:** `scripts/release/compare_profiles.py <anterior> <novo>` aceita a pasta dos perfis, um
> stage ou um AppImage extraído, e sai com 1 e a lista dos arquivos quando os perfis mudam. Rode antes de cada
> release; a regra abaixo continua valendo.

13. Com `-fprofile-use`, o ccache inclui o perfil no hash. Se os perfis forem retreinados a cada versão, os 84
    chunks PPU (a maior parte do tempo) recompilam sempre.
14. A regra: **retreinar só quando o código traduzido mudar** (lifter, headers que entram nos chunks PPU, flags dos
    chunks, versão do GCC). Nas outras versões, distribuir os mesmos arquivos de perfil, byte a byte.
    - Um script de release compara o hash dos perfis com os da versão anterior e avisa se mudaram sem motivo.
15. As fases 1 a 5, a M3 e a M4 deste plano mudam o codegen. Agrupe essas mudanças em versões que já exigiriam
    retreinar, em vez de espalhá-las por várias versões.

### U5: validar

16. Medir em três cenários, nos dois sistemas (AppImage no Linux e pacote no Windows):

    | O que mudou entre as versões | Hoje | Esperado |
    |---|---|---|
    | Só documentação, testes ou launcher | 15–60 min | **0** (nada recompila) |
    | Um bridge em `port/` (por exemplo `vulkan_bridge.c`) | 15–60 min | **alguns minutos** (só esses arquivos mais o link de cerca de 90 MB) |
    | Lifter, headers PPU, flags ou perfis | 15–60 min | 15–60 min (inevitável) |

17. Conferir que o executável incremental é idêntico ao de um build limpo com as mesmas entradas, ou ao menos que o
    `bench_combat.py` dá o mesmo resultado.
18. Conferir que mover a pasta de dados para outro PC (outra CPU) recompila e não usa o cache da CPU anterior.

Os tempos esperados são estimativas e ainda não foram medidos.

### U6: primeira compilação mais curta, com `-O1` no código frio (1–2 dias) [precisa medir]

A U1 a U5 cuidam das atualizações; esta ataca a primeira instalação. Quase todo o tempo está nos 84 arquivos PPU
traduzidos, compilados com `-O2`, mas o profile mostra que o tempo de CPU do jogo se concentra em poucos chunks (015,
021 e 029 somam boa parte do `runTaskMain`, Parte 2).

19. **Medir onde vai o tempo de compilação:** o `.ninja_log` de um build completo dá o tempo de cada objeto. Listar os
    chunks PPU por tempo de compilação ao lado da fração de tempo de CPU que cada um tem no `PORT_PROFILE` da STG02.
20. **Usar os perfis de PGO para separar quente e frio.** Um chunk cujas funções somam quase zero de contagem no
    `.gcda` é frio. O CMake compila os frios com `-O1` (ou `-Og`) e os quentes com `-O2`, por uma lista gerada a
    partir dos perfis e distribuída junto com eles (`usr/share/battride/pgo/`), sem código do jogo.
21. **Regra de adoção:** a mesma da Parte 3, com três pares intercalados no `bench_combat.py`, mais o tempo total do
    build. A mudança só entra se o fps não piorar.
22. **Cuidado com os dados de cobertura:** o treino só passa pelo início do jogo (menus, tutorial e STG02). Um chunk
    frio no treino pode ser quente em outra fase. Validar também o primeiro estágio de cada capítulo da Chronicle com
    o save de história completa, como na 0.5.2.
23. Mudar as flags de um chunk muda o codegen, então os perfis precisam ser retreinados (U4).

O ganho em minutos depende da medição do item 19: se os chunks frios forem a maioria do tempo de compilação, o
`-O1` pode cortar boa parte dele. Nada foi medido ainda.

---

## Parte 11: tamanho dos pacotes (S1 a S3)

### Como está

| | infamous2 v0.6.3 | Battride v0.6.0 |
|---|---|---|
| Windows | `Setup.exe` de 139 KB, que baixa cerca de 50 MB | zip de **707 MB** |
| Linux | zip de 36 MB | AppImage de **235 MB** |
| Fontes de terceiros na release | — | 1,2 GB (Linux) e 2,1 GB (Windows, em duas partes) |

**Por que o infamous2 é pequeno:**

- **A engine vem pronta**, sem código do jogo: `Infamous2-engine-win-x64.zip`, 3,7 MB, com o executável e o
  `SDL2.dll`.
- **O jogador compila só o código traduzido**, como um módulo (`Infamous2Game.dll` ou `.so`). O módulo é compilado em
  modo freestanding (`/X /NODEFAULTLIB` e um header de prelúdio próprio), sem biblioteca C nem headers do Windows.
- **O toolchain é mínimo:** `clang.exe`, `lld.exe`, `llvm-profdata` e 177 headers de intrínsecos (33 MB compactado),
  mais o Python embutido (11 MB). O manifesto `tools/slim_assets.json` lista cada peça com tamanho e SHA-256.

**Por que o Battride é grande.** O `BUNDLE` em `scripts/release/build_windows_package.py` leva estes pacotes do MSYS2
UCRT64, e o pacman puxa todas as dependências deles:

| Item | Por que está lá |
|---|---|
| `gcc` **e** `clang` + `lld` + `binutils` | dois compiladores completos (clang para C, GCC para C++) |
| `cmake`, `ninja`, `pkgconf`, `SDL2`, `vulkan-headers`, `glslang`, `spirv-tools` | o jogador compila o runtime e os bridges inteiros, não só o código do jogo |
| `gtk4`, `libadwaita`, `adwaita-icon-theme`, `python-gobject` | o launcher. No Windows o GTK arrasta cairo, pango, harfbuzz, ICU, librsvg e outros |
| `ffmpeg` | o FFmpeg completo, com todos os codecs e as bibliotecas deles. Os vídeos PAMF (`sail_decoder.hpp`) só precisam do demuxer `mpeg` e de poucos decodificadores |
| `python`, `python-capstone`, `python-pycryptodome` | o lifter e a importação do disco |

Os tarballs de fontes existem para cumprir as licenças dos binários de terceiros e encolhem junto com o pacote.

### S1: cortar o pacote atual, sem mudar o design (dias)

1. **Medir antes de cortar.** Um script em `scripts/release/` soma o tamanho instalado por pacote do MSYS2
   (`pacman -Qi`, "Installed Size", com as dependências atribuídas a quem as puxou) e por diretório do stage. Rodar
   o mesmo no AppImage. A ordem dos itens abaixo depende desse resultado.
2. **Um FFmpeg mínimo.**
   - Compilar um `ffmpeg.exe` estático só com o demuxer `mpeg`, os decodificadores de vídeo e áudio que os PAMF do
     jogo usam, `rawvideo` e `pcm`, e as saídas que o `sail_decoder.hpp` lê.
   - Listar os codecs reais com `ffprobe` sobre os PAMF do disco antes de escolher.
   - O script de build do FFmpeg vai para `scripts/release/` e as fontes para o tarball de fontes (LGPL, sem
     `--enable-gpl`, se nenhum decodificador exigir).
   - **Testar:** o filme de abertura e as cutscenes da STG02, com som sincronizado.
   - É provavelmente o maior corte.
3. **Um compilador só.**
   - Os perfis de PGO são do GCC, então tente compilar o C com GCC também e tire `clang`, `lld` e as bibliotecas
     do LLVM.
   - Antes, descubra por que o C usa clang hoje (alguma extensão que o SDK exige?). Medir o desempenho depois, porque
     a parte em C do runtime muda de compilador.
   - Se o clang for indispensável, o caminho oposto (clang em tudo) só vale junto com a fase 5 do plano, por causa
     do PGO.
4. **Launcher leve no Windows.**
   - Trocar GTK 4, libadwaita e PyGObject por tkinter (vem com o Python) ou por um executável nativo pequeno. Isso
     combina com o `.exe` assinado da W2.
   - A lógica do `packaging/importer.py` fica igual: só a interface muda.
   - No AppImage, avaliar usar o GTK 4 do sistema quando ele existir. Precisa ser testado nas distribuições da
     tabela de requisitos.
5. **Limpar o resto:** tirar `test/`, `idlelib`, `ensurepip` e `__pycache__` do Python; docs, testes e `third_party`
   do SDK que o build não lê; ícones que o launcher não mostra; executáveis do MSYS2 que nada chama. Isso também
   ajuda no limite de 260 caracteres da W1.
6. **Compressão melhor:** o zip usa `-mx=5` (deflate). O instalador Inno Setup da 0.6.1, com LZMA2 sólido, já sai
   menor com o mesmo conteúdo. Mantenha o zip, mas compare os dois tamanhos.
7. **Validar cada corte** com o build completo e a STG02 nos dois sistemas. Um script de release confere que todo
   executável chamado pelo `importer.py`, pelo CMake e pelo `generate.py` está no pacote.

### S2: baixar sob demanda, como o `Setup.exe` (cerca de 1 semana)

8. **Um manifesto por release** (como o `slim_assets.json`), com cada componente, versão, tamanho, SHA-256 e para
   onde ele vai: toolchain, Python, FFmpeg, launcher e port.
9. **Cada componente vira um asset da release.** O launcher pequeno baixa só o que falta ou mudou e confere o
   SHA-256 antes de usar.
10. **Componentes que não mudaram ficam na máquina.** Uma atualização que só mexe no port baixa alguns MB. Junto com
    a trilha U (build incremental), atualizar fica barato em download e em tempo.
11. **Sem internet:** um pacote completo continua disponível, para quem quiser instalar offline.
12. **Licenças:** os componentes continuam vindo de versões fixadas, com as fontes correspondentes na release.

### S3: engine pronta mais módulo do jogo, o modelo do infamous2 (semanas)

13. **O que muda:** o runtime, os bridges, o SDK, SDL2, glslang e o backend Vulkan vão compilados na release, sem
    código do jogo. O jogador compila só o PPU e o SPU gerados como DLL ou `.so`, com headers mínimos. CMake, Ninja,
    SDL2 de desenvolvimento, headers do Vulkan e a sysroot completa saem do pacote.
14. **Custos reais:**
    - os fast paths que hoje dependem do linker (`store_wrap.c` via `--wrap`, o `port_ppu_memory.h` e os globais como
      `vm_base` e `port_ppu_store_end`) passam a cruzar a fronteira entre DLL e executável. Isso cria uma indireção
      extra e **conflita com a M3** (base da memória como constante). Uma saída é o módulo receber, na carga, um bloco
      de ponteiros e constantes que ele guarda em globais próprios;
    - a engine não pode usar `-march=native`: distribua variantes `x86-64-v2` e `x86-64-v3` e deixe o `-march=native`
      só para o código do jogo, onde ele pesa;
    - a ABI entre engine e módulo precisa de versão e de uma checagem de layout na carga (como o `"abi": 2` e o
      `layout` do manifesto do infamous2);
    - o PGO passa a ter duas partes: a da engine, treinada por você e já aplicada no binário distribuído, e a do
      módulo, que continua como hoje.
15. **Ordem:** só depois das fases 1 a 3 e da M3, porque elas mudam justamente o acesso à memória que atravessaria
    a fronteira. Medir o `busy_ms` (F1) antes e depois: a separação não pode custar desempenho.

### O que esperar

- **S1** deve cortar boa parte dos 707 MB, mas o tamanho final depende da medição do item 1.
- **S2** muda pouco a primeira instalação e muito as atualizações.
- **S1 a S3 juntos** chegam perto dos cerca de 50 MB do infamous2.

Esses números são estimativas: nada foi medido no pacote do Battride.

---

## Parte 12: interface do launcher (L1)

### Como está na v0.6.0 (`packaging/launcher.py`)

As cinco páginas (Game, Settings, Keyboard, Controller e Log) ficam num `Adw.ViewStack`. A troca entre elas é feita
por abas no cabeçalho (`Adw.ViewSwitcherTitle`) e, quando a janela fica estreita, por uma barra embaixo
(`Adw.ViewSwitcherBar`). O botão **Install & Play** e o de créditos ficam no cabeçalho, e a barra de progresso com a
última mensagem fica embaixo de todas as páginas.

### L1: páginas numa sidebar à esquerda (cerca de 1 dia)

Trocar as abas por uma **sidebar fixa à esquerda**, com ícone e nome de cada página, e o conteúdo da página à direita.

1. **Widgets compatíveis com o libadwaita 1.1**, o mínimo do AppImage (Ubuntu 22.04). O `Adw.NavigationSplitView` e
   o `Adw.OverlaySplitView` só existem a partir do 1.4, então não servem. Use:
   - um `Gtk.Stack` no lugar do `Adw.ViewStack`;
   - à esquerda, um `Gtk.ListBox` com a classe de estilo `navigation-sidebar`, uma linha por página (ícone e nome,
     os mesmos de hoje); ou o `Gtk.StackSidebar` do GTK 4.0, se o visual dele bastar;
   - os dois lados dentro de um `Adw.Leaflet` (existe desde o 1.0), que mostra só a página e um botão de voltar
     quando a janela fica estreita.
2. **O cabeçalho** perde as abas e mostra o nome da página aberta. **Install & Play** e o botão de créditos ficam
   onde estão. A barra de progresso continua embaixo do conteúdo, visível em qualquer página.
3. **A página Log** pode mostrar na sidebar um indicador (um ponto ou um contador) quando chega um erro enquanto o
   jogador está em outra página.
4. **Manter o que já existe:** a opção `--page` (usada pelos screenshots de teste), a página inicial `game` e a página
   aberta lembrada nas configurações, se for o caso.
5. **Tamanho da janela:** hoje 880 × 780. Com uma sidebar de cerca de 200 px, conferir se as páginas Settings e
   Keyboard cabem sem rolagem horizontal; aumentar a largura padrão se precisar.
6. **Windows:** o pacote usa o GTK e o libadwaita do MSYS2, que são mais novos, então o mesmo código serve. Se a S1.4
   (launcher leve no Windows, sem GTK) for adotada, a nova interface precisa repetir a sidebar. Decida a S1.4 antes
   de começar a L1, para não fazer a interface duas vezes.

### Validar

- As cinco páginas abrem pela sidebar e pela opção `--page`, no AppImage e no pacote Windows.
- Janela estreita: a sidebar recolhe e o botão de voltar aparece.
- Tema claro e escuro; teclado (Tab e setas) navega pela sidebar.
- Durante uma importação ou um build, trocar de página não interrompe a barra de progresso nem o log.
- Screenshots novos para o README e o `docs/APPIMAGE.md`.

---

## Parte 13: ordem única de execução

| # | Fase | Ganho esperado | Esforço |
|---|---|---|---|
| 1 | **Fase 0**: lifter vendorizado, switches desligados | (base) | cerca de 1 dia |
| 2 | **F1**: métrica `busy_ms` | (para medir) | cerca de 1 dia |
| 3 | **F2**: uncap e teste de velocidade | define se a F3 existe | 1–2 dias |
| 4 | **M3**: base constante (hidden, no-pie, endereço fixo) | 2–5% | horas a 2 dias |
| 5 | **M5**: timebase com `rdtsc` | 0%, ou 10%+ com HPET | horas |
| 6 | **G1**: anisotrópico | visual alto, custo quase nulo | horas |
| 7 | **Fase 1 / M4**: registradores locais | o maior, de 15% a 25% | 1–2 dias (port) ou 1–2 semanas (próprio) |
| 8 | **M8 e M9**: profiling do HLE e da espera pelo fence | define o próximo passo | cerca de 1 dia |
| 9 | **M1**: cache validado de `func_004B5120` | até ~10% | 2–3 dias |
| 10 | **Fase 2**: VMX e ponto flutuante | médio | 1–2 dias |
| 11 | **G2**: SMAA e nitidez | visual alto | 1–2 dias |
| 12 | **Fase 3**: stores e loads leves | alto (12–20% de memória) | 2–4 dias |
| 13 | **M2**: substituições nativas (`func_005B2930` primeiro) | 5–15%, aos poucos | contínuo |
| 14 | **G4**: sombras maiores | visual médio a alto | 2–4 dias |
| 15 | **Fase 4 e M6**: chamadas | baixo a médio | 1–2 dias |
| 16 | **G3**: MSAA | visual médio, caro na GPU | 2–3 dias |
| 17 | **G5**: efeitos em resolução cheia | depende do jogo | 1–3 dias |
| 18 | **Fase 5 e M7**: clang + ThinLTO, BOLT | 2–8%, difícil prever | 3–8 dias |
| 19 | **M10**: CPUs pequenas | só em CPUs fracas | horas |
| 20 | **G7**: pacotes de texturas | indireto (mods) | 1–2 dias |
| 21 | **G6**: widescreen Hor+ | visual alto | 5+ dias |
| 22 | **F3**: 120/144 fps para o jogador | depende do item 3 | variável |

A ordem prioriza duas coisas: as ferramentas de medição primeiro, e depois os itens baratos de ganho alto. As fases
gráficas podem correr em paralelo às de desempenho, porque mexem em arquivos diferentes (`vulkan_bridge.c` e
`rsx_engine_bridge.c`, não o lifter). Cada fase que muda o codegen exige retreinar o PGO antes da comparação final.

**A trilha do Windows corre à parte**, porque mexe só no empacotamento e no launcher (`packaging/`,
`scripts/release/`), e é ela que define a 0.6.1:

| # | Item | Resolve | Esforço |
|---|---|---|---|
| W-a | **W1.1 a W1.3**: medir, cortar e encurtar os caminhos | o limite de 260 caracteres, na causa | 1–2 dias |
| W-b | **W1, contra uso errado**: `.cmd` que detecta o zip | o jogo rodado sem extrair | horas |
| W-c | **Instalador Inno Setup** (`docs/ROADMAP.md`) | pasta curta e checagem antes de copiar | cerca de 1,5 dia |
| W-d | **W1.4**: junction automática | caminhos que continuarem longos | 1 dia |
| W-e | **W3**: matriz de teste no Windows 10 e 11 | "Windows 10 needs testing" | 1–2 dias |
| W-f | **W2.1 e W2.2**: assinatura e launcher `.exe` | SmartScreen | 1–3 dias, mais a espera pelo certificado |
| W-g | **W2.3**: teste do hospedeiro assinado com DLL | Smart App Control | 1–2 dias |
| W-h | **W4**: validação da 0.6.1 | a release | cerca de 1 dia |

**A trilha de atualizações** também mexe só em `packaging/` e `scripts/`, e **entra na 0.6.1** junto com a do
Windows (decidido em 19/11). O jogador não tem como deixar de compilar na instalação (Parte 10, "Por que o jogador
sempre compila uma vez"), mas com ela passa a compilar uma vez só, em vez de recompilar por inteiro a cada correção
pequena. A U-a e a U-b vêm primeiro: são as mais baratas e já evitam o rebuild por mudanças de documentação, como a do
`90a666c`.

| # | Item | Resolve | Esforço |
|---|---|---|---|
| U-a | **U1**: identidade só com o que afeta o binário | rebuild por docs, testes, launcher ou versão | horas |
| U-b | **U4**: política dos perfis de PGO | rebuild dos 84 chunks a cada versão | regra de release |
| U-c | **U2**: reaproveitar a tradução | a etapa de tradução em toda atualização | cerca de 1 dia |
| U-d | **U3**: ccache separado por CPU | a recompilação completa quando só os bridges mudam | 1–2 dias |
| U-e | **U5**: validar os três cenários | — | cerca de 1 dia |
| U-f | **U6**: `-O1` nos chunks PPU frios, guiado pelo PGO | o tempo da primeira compilação | 1–2 dias, mais medição; pode ficar para depois da 0.6.1 se a medição não mostrar ganho |

**A trilha de tamanho** também mexe só em `packaging/` e `scripts/release/`, menos a S3, que mexe na arquitetura:

| # | Item | Resolve | Esforço |
|---|---|---|---|
| S-a | **S1.1**: medir o tamanho por pacote e por diretório | define as prioridades | horas |
| S-b | **S1.2**: FFmpeg mínimo | provavelmente o maior corte | 1–2 dias |
| S-c | **S1.4**: launcher leve no Windows (junto com a W2) | GTK, ICU e dependências | 2–4 dias |
| S-d | **S1.3**: um compilador só | LLVM ou GCC duplicado | 1–2 dias, mais medição |
| S-e | **S1.5 a S1.7**: limpeza, compressão e validação | o resto | 1–2 dias |
| S-f | **S2**: download sob demanda | o download das atualizações | cerca de 1 semana |
| S-g | **S3**: engine pronta mais módulo do jogo | o toolchain completo no pacote | semanas, depois das fases 1 a 3 e da M3 |

**A trilha do launcher** mexe só no `packaging/launcher.py` e entra **na 0.6.1** (pedido em 19/11). Ela depende
da decisão sobre a S1.4: se o launcher do Windows deixar de usar GTK, a sidebar precisa ser feita nas duas
interfaces.

| # | Item | Resolve | Esforço |
|---|---|---|---|
| L-a | **L1**: páginas numa sidebar à esquerda | navegação entre Game, Settings, Keyboard, Controller e Log | cerca de 1 dia |
