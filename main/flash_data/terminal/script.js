/* Терминал шлюза: экран xterm, настройки порта в подвале, состояние — там же.
 *
 * Правила страницы:
 *   - в терминал попадают ТОЛЬКО данные порта; служебные сообщения живут в
 *     статусной строке подвала;
 *   - настройки порта выбираются ДО подключения, их применяет кнопка
 *     «Подключиться»; пока сеанс жив, списки заблокированы;
 *   - если настройки порта сменил кто-то другой (страница конфигурации,
 *     скрипт), терминал сообщает об этом, показывает новые настройки и рвёт
 *     сеанс: работать на других настройках нельзя. */
class ESP32Terminal {
    constructor() {
        this.term = null;
        this.socket = null;
        this.isConnected = false;
        this.isRegistered = false;
        this.packetCount = 0;
        this.subscriptionSid = null;     /* подписка на приём порта */
        this.configSid = null;           /* подписка на настройки порта */
        /* Порт — 2 (uart2) или 1 (uart1); имя FID собирается из него */
        this.port = 2;
        /* Местное эхо: показывать набранное сразу, не дожидаясь эхо линии */
        this.localEcho = (localStorage.getItem('localEcho') !== '0');
        /* Настройки, которые мы применили: с ними сравниваем то, что приходит
         * от устройства, чтобы заметить чужое вмешательство. */
        this.applied = null;
        this.cfgSid = Math.floor(Math.random() * 0xffff) + 1;
        this.initTerminal();
        this.bindEvents();
        this.loadSettings();
    }

    /* Имя сервиса для текущего канала: uart1.* или uart2.* */
    fid(action) {
        return `uart${this.port}.${action}`;
    }

    initTerminal() {
        this.term = new Terminal({
            cursorBlink: true,
            cursorStyle: 'block',
            fontSize: 14,
            fontFamily: 'Consolas, "Courier New", monospace',
            theme: {
                background: '#0a0a0f',
                foreground: '#e0e0e0',
                cursor: '#4fc3f7',
                black: '#000000',
                red: '#ff5252',
                green: '#69f0ae',
                yellow: '#ffd740',
                blue: '#448aff',
                magenta: '#e040fb',
                cyan: '#18ffff',
                white: '#ffffff',
                brightBlack: '#616161',
                brightRed: '#ff867c',
                brightGreen: '#b9f6ca',
                brightYellow: '#ffff6e',
                brightBlue: '#82b1ff',
                brightMagenta: '#ea80fc',
                brightCyan: '#a7ffeb',
                brightWhite: '#f5f5f5'
            }
        });

        this.fitAddon = new FitAddon.FitAddon();
        this.term.loadAddon(this.fitAddon);

        const terminalElement = document.getElementById('terminal');
        terminalElement.innerHTML = '';
        this.term.open(terminalElement);

        setTimeout(() => {
            try {
                this.fitAddon.fit();
                this.term.focus();
                this.term.refresh(0, this.term.rows - 1);
            } catch (error) {
                console.error('Ошибка при инициализации терминала:', error);
            }
        }, 100);

        this.term.onData((data) => {
            this.handleUserInput(data);
        });
    }

    bindEvents() {
        document.getElementById('connect-btn').addEventListener('click', () => {
            this.isConnected ? this.disconnect('отключено пользователем') : this.connect();
        });

        // Кнопка местного эха: только показ набранного, к прибору не относится
        const echoBtn = document.getElementById('echo-btn');
        this.renderEchoButton();
        echoBtn.addEventListener('click', () => {
            this.localEcho = !this.localEcho;
            localStorage.setItem('localEcho', this.localEcho ? '1' : '0');
            this.renderEchoButton();
            this.term.focus();
        });

        // Порт выбирается только ДО подключения
        document.getElementById('port-sel').addEventListener('change', () => {
            this.port = parseInt(document.getElementById('port-sel').value, 10) || 2;
            this.showChannel();
        });

        window.addEventListener('resize', () => {
            if (this.fitAddon) this.fitAddon.fit();
        });
    }

    renderEchoButton() {
        const btn = document.getElementById('echo-btn');
        btn.textContent = this.localEcho ? 'Эхо: вкл' : 'Эхо: выкл';
        btn.classList.toggle('on', this.localEcho);
    }

    /* Настройки порта помним между открытиями страницы. Без сохранённых —
     * разумные умолчания (115200 8N1), а не первый пункт списка: иначе
     * подключение настраивало бы порт на 1200 бод. */
    loadSettings() {
        let saved = null;
        try { saved = JSON.parse(localStorage.getItem('termSettings') || 'null'); }
        catch (e) { saved = null; }

        const put = (id, v) => {
            const el = document.getElementById(id);
            if (el && v !== undefined && v !== null) el.value = String(v);
        };
        if (saved) {
            put('baud-sel', saved.br);
            put('bits-sel', saved.wl);
            put('par-sel', saved.par);
            put('stop-sel', saved.sb);
            put('mode-sel', saved.mode);
            if (saved.port) {
                this.port = saved.port;
                put('port-sel', saved.port);
            }
        } else {
            put('baud-sel', 115200);
        }
        this.showChannel();
    }

    saveSettings(cfg) {
        try {
            localStorage.setItem('termSettings', JSON.stringify({
                br: cfg.BR, wl: cfg.WL, par: cfg.PAR, sb: cfg.SB,
                mode: cfg.MODE, port: this.port,
            }));
        } catch (e) { /* приватный режим — просто не запомним */ }
    }

    /* Настройки из списков в виде ARG сервиса uartN.config. */
    settingsFromUi() {
        const num = id => parseInt(document.getElementById(id).value, 10);
        return {
            BR: num('baud-sel'),
            WL: num('bits-sel'),
            PAR: num('par-sel'),
            SB: num('stop-sel'),
            MODE: num('mode-sel'),
        };
    }

    /* Настройки в том виде, в каком их присылает устройство: биты данных оно
     * отдаёт кодом перечисления (1 — восемь, 0 — семь), а в списке стоят сами
     * биты (8 и 7). Без приведения «свои» настройки не совпадали бы с ответом,
     * и страница принимала бы собственное применение за чужое вмешательство. */
    settingsAsDevice(cfg) {
        return {
            BR: cfg.BR, PAR: cfg.PAR, SB: cfg.SB, MODE: cfg.MODE,
            WL: (cfg.WL === 8 || cfg.WL === 1) ? 1 : 0,
        };
    }

    /* Строка настроек как её принято писать у последовательных портов: 8N1. */
    formatSettings(cfg) {
        const bits = (cfg.WL === 8 || cfg.WL === 1) ? 8 : 7;
        const par = ['N', 'O', 'E'][cfg.PAR] || '?';
        const stop = (cfg.SB === 3) ? '1.5' : String(cfg.SB);
        /* Режимы: 0 — дуплекс, 1 — полудуплекс (RS-485), 2 — однопроводный. */
        const mode = ['дуплекс', 'полудуплекс', 'однопроводный'][cfg.MODE] || 'дуплекс';
        return `${cfg.BR} бод, ${bits}${par}${stop}, ${mode}`;
    }

    /* ── состояние: всё видно в подвале, терминал не засоряем ─────────────── */

    setState(text, cls) {
        const el = document.getElementById('status-text');
        el.textContent = text;
        el.className = cls || '';
        const res = document.getElementById('channel-name');
        if (res) res.textContent = `uart${this.port}`;
    }

    showChannel() {
        this.setState(this.isConnected ? 'подключено' : 'отключено',
                       this.isConnected ? 'ok' : '');
    }

    setCfgState(text) {
        const el = document.getElementById('cfg-state');
        if (el) el.textContent = text;
    }

    /* Пока сеанс жив, настройки порта менять нельзя: списки заблокированы. */
    setSettingsEnabled(on) {
        for (const id of ['port-sel', 'mode-sel', 'baud-sel', 'bits-sel', 'par-sel', 'stop-sel']) {
            const el = document.getElementById(id);
            if (el) el.disabled = !on;
        }
    }

    /* ── подключение ─────────────────────────────────────────────────────── */

    async connect() {
        /* Адрес не спрашиваем: страница открыта с прибора, значит и WebSocket
         * на нём же (порт 81 — CONFIG_WEB_SOCKET_SERVER_PORT). */
        const url = `ws://${location.hostname}:81/ws`;

        this.setState('подключаемся…', '');
        this.setCfgState('');

        try {
            this.socket = new WebSocket(url);

            this.socket.onopen = () => {
                this.isConnected = true;
                this.setState('подключено', 'ok');
                this.setSettingsEnabled(false);

                /* Сначала применяем выбранные настройки, потом подписываемся на
                 * приём и на изменения настроек. */
                const cfg = this.settingsFromUi();
                this.applied = this.settingsAsDevice(cfg);
                this.applyConfig(cfg);
                this.registerClient();
                this.subscribeConfig();
            };

            this.socket.onmessage = (event) => {
                this.handleWebSocketMessage(event.data);
            };

            this.socket.onclose = () => {
                const wasConnected = this.isConnected;
                this.isConnected = false;
                this.isRegistered = false;
                this.subscriptionSid = null;
                this.configSid = null;
                this.setSettingsEnabled(true);
                document.getElementById('connect-btn').textContent = 'Подключиться';
                if (this.closing) {          /* причину уже написали в disconnect() */
                    this.closing = false;
                } else if (wasConnected) {
                    this.setState('связь оборвана', 'err');
                } else {
                    this.setState('отключено', '');
                }
            };

            /* У события error нет внятного текста, и приходит оно в том числе
             * тогда, когда соединение закрыли МЫ САМИ (после disconnect) —
             * поэтому статус здесь не трогаем: состояние и причину пишет
             * обработчик закрытия или сам disconnect(). */
            this.socket.onerror = () => {};

        } catch (error) {
            this.setState('ошибка подключения', 'err');
        }
    }

    applyConfig(cfg) {
        if (!this.socket) return;
        this.cfgSid = Math.floor(Math.random() * 0xffff) + 1;
        this.socket.send(JSON.stringify({
            FID: this.fid('config'), SID: this.cfgSid, FLAGS: 0, ARG: cfg,
        }));
        this.saveSettings(cfg);
        this.setCfgState(this.formatSettings(cfg));
    }

    /* Подписка на приём порта: данные уходят кадром сразу по приходу, период
     * переспроса — только страховка, если кадр-побудка потеряется. */
    registerClient() {
        this.subscriptionSid = Math.floor(Math.random() * 0xffff) + 1;
        this.txSid = Math.floor(Math.random() * 0xffff) + 1;
        this.socket.send(JSON.stringify({
            FID: this.fid('rx'),
            SID: this.subscriptionSid,
            FLAGS: 4,                       // CALL_FLAG_LONG_TERM
            ARG: { RDL: 200 }
        }));

        this.resubscribeTimer = setTimeout(() => {
            if (!this.isRegistered && this.isConnected) this.registerClient();
        }, 3000);
    }

    /* Подписка на настройки порта: устройство рассылает их подписчикам, когда
     * настройки меняет кто-то другой. Редкий период — страховка. */
    subscribeConfig() {
        this.configSid = Math.floor(Math.random() * 0xffff) + 1;
        this.socket.send(JSON.stringify({
            FID: this.fid('config'),
            SID: this.configSid,
            FLAGS: 4,
            ARG: { RDL: 2000 }
        }));
    }

    sendDelete(fid, sid) {
        if (!this.socket || sid === null) return;
        this.socket.send(JSON.stringify({ FID: fid, SID: sid, FLAGS: 8 }));
    }

    disconnect(reason) {
        /* Причину отключения пишем мы, а не обработчик закрытия: иначе оно
         * затрёт её своим «связь оборвана». */
        this.closing = true;
        clearTimeout(this.resubscribeTimer);
        this.sendDelete(this.fid('rx'), this.subscriptionSid);
        this.sendDelete(this.fid('config'), this.configSid);
        if (this.socket) {
            this.socket.close();
            this.socket = null;
        }
        this.isConnected = false;
        this.isRegistered = false;
        this.subscriptionSid = null;
        this.configSid = null;
        this.setSettingsEnabled(true);
        document.getElementById('connect-btn').textContent = 'Подключиться';
        this.setState(reason || 'отключено', reason ? 'err' : '');
    }

    /* ── приём ───────────────────────────────────────────────────────────── */

    handleWebSocketMessage(data) {
        try {
            const packet = JSON.parse(data);
            this.packetCount++;
            document.getElementById('packet-count').textContent = this.packetCount;
            document.getElementById('last-activity').textContent = new Date().toLocaleTimeString();

            /* Настройки порта: ответ на наше применение, ответ на переспрос
             * подписки или рассылка при чужом вмешательстве — всё приходит одним
             * видом кадра, разбираем по значениям. */
            if (packet.FID === this.fid('config') && packet.ARG
                && packet.ARG.BR !== undefined) {
                this.handleConfig(packet.ARG);
                return;
            }

            if (packet.FID === this.fid('rx') && packet.ARG) {
                if (packet.ARG.STA === "0x00000005") {
                    this.isRegistered = true;
                    clearTimeout(this.resubscribeTimer);
                    document.getElementById('connect-btn').textContent = 'Отключиться';
                } else if (this.isRegistered) {
                    this.processUARTData(packet.ARG);
                }
            }

        } catch (error) {
            console.error('Ошибка обработки сообщения:', error);
        }
    }

    /* Настройки порта пришли от устройства. Если они не те, что мы применили,
     * значит их сменил кто-то другой: продолжать сеанс нельзя — данные пойдут
     * на других настройках, и разбирать их будет нечем. Показываем новые
     * настройки, обновляем списки и отключаемся с причиной в подвале. */
    handleConfig(cfg) {
        const same = this.applied
            && this.applied.BR === cfg.BR && this.applied.WL === cfg.WL
            && this.applied.PAR === cfg.PAR && this.applied.SB === cfg.SB
            && this.applied.MODE === cfg.MODE;

        if (same) {
            this.setCfgState(this.formatSettings(cfg));
            return;
        }

        this.renderSettings(cfg);
        if (!this.isConnected) {
            /* Сеанса нет — просто показываем, что стоит на порту */
            this.applied = this.settingsAsDevice(cfg);
            this.setCfgState(this.formatSettings(cfg));
            return;
        }

        this.term.writeln(`\x1b[33m[настройки порта изменены: ${this.formatSettings(cfg)} — сеанс отключён]\x1b[0m`);
        this.disconnect(`настройки порта изменены (${this.formatSettings(cfg)})`);
    }

    renderSettings(cfg) {
        const set = (id, value) => {
            const el = document.getElementById(id);
            if (el && value !== undefined && value !== null) el.value = String(value);
        };
        set('baud-sel', cfg.BR);
        /* В ответе WL — значение перечисления (0 — 7 бит, 1 — 8 бит) */
        set('bits-sel', (cfg.WL === 8 || cfg.WL === 1) ? 8 : 7);
        set('par-sel', cfg.PAR);
        set('stop-sel', cfg.SB);
        set('mode-sel', cfg.MODE);
        this.applied = this.settingsAsDevice(cfg);
    }

    /* Кадр приёма: {"RX":"<base64>[<спецсимвол><base64>]…"}. В данных два
     * спецсимвола, которых не бывает в base64:
     *   '#' — передача на линии закончилась (idle), кадр закрыт;
     *   '~' — здесь потеряли часть данных.
     * Кусок без разделителя — продолжение предыдущего: длинный кадр приходит
     * частями. Терминалу деление не нужно, он пишет байты подряд, поэтому
     * спецсимволы из потока выбрасываются (по ним кадры делят протоколы на
     * странице). Принимаем и старую форму — голую строку base64. */
    processUARTData(arg) {
        const raw = (typeof arg === 'string') ? arg : (arg.RX || '');
        for (const base64Data of raw.split(/[#~]/)) {
            if (base64Data) this.writeBytes(base64Data);
        }
    }

    writeBytes(base64Data) {
        try {
            const binaryString = atob(base64Data);
            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            this.term.write(new TextDecoder().decode(bytes));
        } catch (error) {
            console.error('Ошибка декодирования Base64:', error);
        }
    }

    /* ── передача ────────────────────────────────────────────────────────── */

    handleUserInput(data) {
        if (!this.isConnected || !this.socket) return;

        /* Местное эхо: на полудуплексной линии ответное эхо приходит только
         * когда мы молчим, поэтому набранное показываем сами. */
        if (this.localEcho) {
            for (const ch of data) {
                if (ch === '\r' || ch === '\n') this.term.write('\r\n');
                else if (ch === '\x7f') this.term.write('\b \b');
                else if (ch >= ' ') this.term.write(ch);
            }
        }

        const encodedData = new TextEncoder().encode(data);
        const base64String = btoa(String.fromCharCode(...encodedData));

        try {
            this.socket.send(JSON.stringify({
                FID: this.fid('tx'),
                SID: this.txSid,
                FLAGS: 0,
                ARG: base64String
            }));
        } catch (error) {
            this.setState('ошибка отправки', 'err');
        }
    }
}
