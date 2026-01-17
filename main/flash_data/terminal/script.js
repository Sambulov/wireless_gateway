class ESP32Terminal {
    constructor() {
        this.term = null;
        this.socket = null;
        //this.fitAddon = null;
        this.isConnected = false;
        this.packetCount = 0;
        this.echoEnabled = false;
        this.initTerminal();
        this.bindEvents();
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
        
        // Инициализация аддона для авто-подгонки размера
        this.fitAddon = new FitAddon.FitAddon();
        this.term.loadAddon(this.fitAddon);
        
        // КРИТИЧЕСКИ ВАЖНО: Правильное открытие терминала
        const terminalElement = document.getElementById('terminal');
        
        // Очищаем контейнер от возможных остаточных элементов
        terminalElement.innerHTML = '';
        
        // Открытие терминала в контейнере
        this.term.open(terminalElement);
        
        // Даем время на рендеринг DOM
        setTimeout(() => {
           try {
                this.fitAddon.fit();
               this.term.focus();
                
               // Форсируем пересчет размеров
               this.term.refresh(0, this.term.rows - 1);
           } catch (error) {
               console.error('Ошибка при инициализации терминала:', error);
           }
        }, 100);
    
        // Открытие терминала в контейнере
        this.term.open(document.getElementById('terminal'));
        this.fitAddon.fit();
        
        // Обработка ввода пользователя
        this.term.onData((data) => {
            this.handleUserInput(data);
        });
        
        this.term.writeln('\x1b[1;36mESP32 Web Terminal v1.0\x1b[0m');
        this.term.writeln('\x1b[33mВведите адрес WebSocket и нажмите "Подключиться"\x1b[0m');
    }
    
    bindEvents() {
        // Кнопка подключения
        document.getElementById('connect-btn').addEventListener('click', () => {
            this.isConnected ? this.disconnect() : this.connect();
        });
        
        // Кнопка очистки
        document.getElementById('clear-btn').addEventListener('click', () => {
            this.term.clear();
        });

        // Кнопка Echo (loopback)
        document.getElementById('echo-btn').addEventListener('click', () => {
            this.toggleEcho();
        });
        
        // Автоподключение при нажатии Enter в поле URL
        document.getElementById('ws-url').addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                if (!this.isConnected) this.connect();
            }
        });
        
        // Обработка сочетаний клавиш
        this.term.attachCustomKeyEventHandler((event) => {
            // Ctrl+C - отправка break
            if (event.ctrlKey && event.key === 'c' && this.term.hasSelection()) {
                return false; // Разрешить стандартное копирование
            }
            
            if (event.ctrlKey && event.key === 'c') {
                this.sendBreakSignal();
                return false;
            }
            
            // Ctrl+L - очистка экрана
            if (event.ctrlKey && event.key === 'l') {
                this.term.clear();
                return false;
            }
            
            return true;
        });
        
        // Автоматическая подгонка размера при изменении окна
        window.addEventListener('resize', () => {
            if (this.fitAddon) this.fitAddon.fit();
        });
    }
    
    async connect() {
        const url = document.getElementById('ws-url').value;
        if (!url) {
            this.showError('Введите URL WebSocket сервера');
            return;
        }
        
        this.updateStatus('Подключение...', 'yellow');
        
        try {
            this.socket = new WebSocket(url);
            
            this.socket.onopen = () => {
                this.isConnected = true;
                this.updateStatus('Подключено', 'green');
                this.updateConnectionStatus('connected');
                
                // Регистрация клиента для получения данных
                this.registerClient();
                
                this.term.writeln('\x1b[1;32m✓ Подключение установлено\x1b[0m');
                this.term.writeln('\x1b[33mОтправка регистрационной команды...\x1b[0m');
            };
            
            this.socket.onmessage = (event) => {
                this.handleWebSocketMessage(event.data);
            };
            
            this.socket.onclose = () => {
                this.isConnected = false;
                this.updateStatus('Отключено', 'red');
                this.updateConnectionStatus('disconnected');
                this.term.writeln('\x1b[1;31m✗ Соединение закрыто\x1b[0m');
            };
            
            this.socket.onerror = (error) => {
                this.showError(`Ошибка WebSocket: ${error.message}`);
                this.updateStatus('Ошибка', 'red');
            };
            
        } catch (error) {
            this.showError(`Ошибка подключения: ${error.message}`);
        }
    }
    
    disconnect() {
        if (this.socket) {
            this.socket.close();
            this.socket = null;
        }
        this.isConnected = false;
        this.echoEnabled = false;
        const echoBtn = document.getElementById('echo-btn');
        echoBtn.textContent = 'Echo: OFF';
        echoBtn.style.background = '';
        this.updateStatus('Отключено', 'red');
        this.updateConnectionStatus('disconnected');
    }
    
    registerClient() {
        // Отправка команды регистрации клиента
        const registerCommand = {
            FID: "0x00001021"
        };
        
        this.socket.send(JSON.stringify(registerCommand));
        this.term.writeln('\x1b[32m✓ Регистрация отправлена\x1b[0m');
    }
    
    handleWebSocketMessage(data) {
        try {
            const packet = JSON.parse(data);
            this.packetCount++;
            document.getElementById('packet-count').textContent = this.packetCount;
            document.getElementById('last-activity').textContent = new Date().toLocaleTimeString();
            
            // Обработка пакетов с данными из UART
            if (packet.FID === "0x00001021" && packet.ARG && !packet.ARG.STA) {
                this.processUARTData(packet.ARG);
            }
            
        } catch (error) {
            console.error('Ошибка обработки сообщения:', error);
        }
    }
    
    processUARTData(base64Data) {
        try {
            // Декодирование из Base64
            const binaryString = atob(base64Data);
            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            // Преобразование в текст
            const decoder = new TextDecoder();
            const text = decoder.decode(bytes);
            // Вывод в терминал
            this.term.write(text);

            // Echo back to UART if enabled
            if (this.echoEnabled && this.isConnected && this.socket) {
                const command = {
                    FID: "0x00001022",
                    ARG: base64Data
                };
                this.socket.send(JSON.stringify(command));
            }
        } catch (error) {
            console.error('Ошибка декодирования Base64:', error);
            this.term.write(`\x1b[31m[Ошибка декодирования]\x1b[0m `);
        }
    }
    
    handleUserInput(data) {
        if (!this.isConnected || !this.socket) {
            this.showError('Не подключено к серверу');
            return;
        }
                
        // Кодирование в Base64
        const encoder = new TextEncoder();
        const encodedData = encoder.encode(data);
        const base64String = btoa(String.fromCharCode(...encodedData));
        
        // Формирование команды для отправки
        const command = {
            FID: "1022",
            ARG: base64String
        };
        
        // Отправка на сервер
        try {
            this.socket.send(JSON.stringify(command));
        } catch (error) {
            this.showError(`Ошибка отправки: ${error.message}`);
        }
    }
    
    sendBreakSignal() {
        // Отправка сигнала Break (Ctrl+C)
        if (this.isConnected && this.socket) {
            const breakCommand = {
                FID: "1022",
                ARG: btoa('\x03') // Ctrl+C
            };
            this.socket.send(JSON.stringify(breakCommand));
            this.term.writeln('\x1b[33m[Отправлен Break]\x1b[0m');
        }
    }

    toggleEcho() {
        this.echoEnabled = !this.echoEnabled;

        const btn = document.getElementById('echo-btn');
        btn.textContent = this.echoEnabled ? 'Echo: ON' : 'Echo: OFF';
        btn.style.background = this.echoEnabled ? '#4caf50' : '';

        this.term.writeln(`\x1b[33m[Echo ${this.echoEnabled ? 'включен' : 'выключен'}]\x1b[0m`);
    }
    
    updateStatus(message, color = 'white') {
        const statusEl = document.getElementById('status');
        const colors = {
            green: '#4caf50',
            red: '#f44336',
            yellow: '#ffeb3b',
            white: '#ffffff'
        };
        
        statusEl.textContent = message;
        statusEl.style.color = colors[color] || colors.white;
        statusEl.className = color === 'green' ? 'status connected' : 'status';
    }
    
    updateConnectionStatus(status) {
        const statusEl = document.getElementById('connection-status');
        const btn = document.getElementById('connect-btn');
        
        if (status === 'connected') {
            statusEl.textContent = 'Подключено';
            statusEl.className = 'connected';
            btn.textContent = 'Отключиться';
            btn.style.background = 'linear-gradient(135deg, #ef5350 0%, #d32f2f 100%)';
        } else {
            statusEl.textContent = 'Отключено';
            statusEl.className = '';
            btn.textContent = 'Подключиться';
            btn.style.background = 'linear-gradient(135deg, #5c6bc0 0%, #3949ab 100%)';
        }
    }
    
    showError(message) {
        this.term.writeln(`\x1b[1;31m[ОШИБКА] ${message}\x1b[0m`);
    }
}
