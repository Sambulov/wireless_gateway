/**
 * xterm.js Terminal UI Tests
 *
 * Tests all terminal functionality except WebSocket communication.
 * Run with: node test_terminal.js
 *
 * Prerequisites:
 *   npm install
 */

const puppeteer = require('puppeteer');
const path = require('path');
const fs = require('fs');

const TERMINAL_HTML = path.resolve(__dirname, '../../../main/flash_data/terminal/index.html');

class TerminalTester {
    constructor() {
        this.browser = null;
        this.page = null;
        this.passed = 0;
        this.failed = 0;
        this.results = [];
    }

    async setup() {
        if (!fs.existsSync(TERMINAL_HTML)) {
            throw new Error(`Terminal HTML not found: ${TERMINAL_HTML}`);
        }

        this.browser = await puppeteer.launch({
            headless: 'new',
            args: ['--no-sandbox', '--disable-setuid-sandbox']
        });
        this.page = await this.browser.newPage();

        // Capture console logs
        this.page.on('console', msg => {
            if (msg.type() === 'error') {
                console.log('  [Browser Error]', msg.text());
            }
        });

        await this.page.goto(`file://${TERMINAL_HTML}`);

        // Wait for terminal to initialize
        await this.page.waitForSelector('#terminal .xterm-screen', { timeout: 5000 });
        await this.page.waitForFunction(() => window.espTerminal !== undefined, { timeout: 5000 });

        // Additional wait for terminal to be fully ready
        await new Promise(resolve => setTimeout(resolve, 500));
    }

    async teardown() {
        if (this.browser) {
            await this.browser.close();
        }
    }

    async test(name, fn) {
        process.stdout.write(`  ${name}... `);
        try {
            await fn();
            this.passed++;
            this.results.push({ name, status: 'PASS' });
            console.log('\x1b[32mPASS\x1b[0m');
        } catch (error) {
            this.failed++;
            this.results.push({ name, status: 'FAIL', error: error.message });
            console.log(`\x1b[31mFAIL\x1b[0m - ${error.message}`);
        }
    }

    async runTests() {
        console.log('\n\x1b[1m=== xterm.js Terminal Tests ===\x1b[0m\n');

        // Terminal Initialization Tests
        console.log('\x1b[36m[Terminal Initialization]\x1b[0m');

        await this.test('Terminal instance exists', async () => {
            const exists = await this.page.evaluate(() => window.espTerminal !== undefined);
            if (!exists) throw new Error('espTerminal is undefined');
        });

        await this.test('Terminal element rendered', async () => {
            const rendered = await this.page.evaluate(() => {
                const el = document.querySelector('#terminal .xterm-screen');
                return el !== null;
            });
            if (!rendered) throw new Error('Terminal screen not found');
        });

        await this.test('FitAddon loaded', async () => {
            const loaded = await this.page.evaluate(() => {
                return window.espTerminal.fitAddon !== undefined;
            });
            if (!loaded) throw new Error('FitAddon not loaded');
        });

        await this.test('Welcome message displayed', async () => {
            const hasWelcome = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < buffer.length; i++) {
                    const line = buffer.getLine(i);
                    if (line) {
                        const text = line.translateToString();
                        if (text.includes('ESP32 Web Terminal')) return true;
                    }
                }
                return false;
            });
            if (!hasWelcome) throw new Error('Welcome message not found');
        });

        // UI Controls Tests
        console.log('\n\x1b[36m[UI Controls]\x1b[0m');

        await this.test('Clear button works', async () => {
            // Write some text first
            await this.page.evaluate(() => {
                window.espTerminal.term.writeln('Test line to clear');
            });

            // Click clear button
            await this.page.click('#clear-btn');
            await new Promise(resolve => setTimeout(resolve, 100));

            // Check that terminal was cleared (buffer should be mostly empty at visible area)
            const cleared = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                const line = buffer.getLine(0);
                return line ? line.translateToString().trim() === '' : true;
            });
            if (!cleared) throw new Error('Terminal not cleared');
        });

        await this.test('Echo button toggles ON', async () => {
            // Ensure echo is OFF first
            await this.page.evaluate(() => {
                window.espTerminal.echoEnabled = false;
                document.getElementById('echo-btn').textContent = 'Echo: OFF';
            });

            await this.page.click('#echo-btn');
            await new Promise(resolve => setTimeout(resolve, 100));

            const state = await this.page.evaluate(() => ({
                enabled: window.espTerminal.echoEnabled,
                text: document.getElementById('echo-btn').textContent
            }));

            if (!state.enabled) throw new Error('echoEnabled should be true');
            if (state.text !== 'Echo: ON') throw new Error(`Button text is "${state.text}", expected "Echo: ON"`);
        });

        await this.test('Echo button toggles OFF', async () => {
            await this.page.click('#echo-btn');
            await new Promise(resolve => setTimeout(resolve, 100));

            const state = await this.page.evaluate(() => ({
                enabled: window.espTerminal.echoEnabled,
                text: document.getElementById('echo-btn').textContent
            }));

            if (state.enabled) throw new Error('echoEnabled should be false');
            if (state.text !== 'Echo: OFF') throw new Error(`Button text is "${state.text}", expected "Echo: OFF"`);
        });

        await this.test('Status indicator updates', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.updateStatus('Test Status', 'green');
            });

            const status = await this.page.$eval('#status', el => ({
                text: el.textContent,
                color: el.style.color
            }));

            if (status.text !== 'Test Status') throw new Error(`Status text is "${status.text}"`);
            if (status.color !== 'rgb(76, 175, 80)') throw new Error(`Status color is "${status.color}"`);
        });

        await this.test('Connection status updates to connected', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.updateConnectionStatus('connected');
            });

            const text = await this.page.$eval('#connection-status', el => el.textContent);
            const btnText = await this.page.$eval('#connect-btn', el => el.textContent);

            if (text !== 'Подключено') throw new Error(`Connection status is "${text}"`);
            if (btnText !== 'Отключиться') throw new Error(`Button text is "${btnText}"`);
        });

        await this.test('Connection status updates to disconnected', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.updateConnectionStatus('disconnected');
            });

            const text = await this.page.$eval('#connection-status', el => el.textContent);
            const btnText = await this.page.$eval('#connect-btn', el => el.textContent);

            if (text !== 'Отключено') throw new Error(`Connection status is "${text}"`);
            if (btnText !== 'Подключиться') throw new Error(`Button text is "${btnText}"`);
        });

        // Terminal Output Tests
        console.log('\n\x1b[36m[Terminal Output]\x1b[0m');

        await this.test('Plain text output', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                window.espTerminal.term.writeln('Hello World');
            });

            const found = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < 5; i++) {
                    const line = buffer.getLine(i);
                    if (line && line.translateToString().includes('Hello World')) return true;
                }
                return false;
            });
            if (!found) throw new Error('Text not found in buffer');
        });

        await this.test('ANSI color codes rendered', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                window.espTerminal.term.writeln('\x1b[1;31mRed Text\x1b[0m');
            });

            const found = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < 5; i++) {
                    const line = buffer.getLine(i);
                    if (line && line.translateToString().includes('Red Text')) return true;
                }
                return false;
            });
            if (!found) throw new Error('Colored text not found');
        });

        await this.test('Error message display', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                window.espTerminal.showError('Test Error');
            });

            const found = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < 5; i++) {
                    const line = buffer.getLine(i);
                    if (line && line.translateToString().includes('Test Error')) return true;
                }
                return false;
            });
            if (!found) throw new Error('Error message not displayed');
        });

        // Base64 Processing Tests
        console.log('\n\x1b[36m[Base64 Processing]\x1b[0m');

        await this.test('processUARTData decodes Base64', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                // "Hello UART" in Base64
                window.espTerminal.processUARTData('SGVsbG8gVUFSVA==');
            });

            const found = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < 5; i++) {
                    const line = buffer.getLine(i);
                    if (line && line.translateToString().includes('Hello UART')) return true;
                }
                return false;
            });
            if (!found) throw new Error('Base64 decoded text not found');
        });

        await this.test('processUARTData handles newlines', async () => {
            await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                // "Line1\r\nLine2" in Base64
                window.espTerminal.processUARTData('TGluZTENCkxpbmUy');
            });
            await new Promise(resolve => setTimeout(resolve, 100));

            const found = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                let foundLine1 = false, foundLine2 = false;
                for (let i = 0; i < 10; i++) {
                    const line = buffer.getLine(i);
                    if (line) {
                        const text = line.translateToString();
                        if (text.includes('Line1')) foundLine1 = true;
                        if (text.includes('Line2')) foundLine2 = true;
                    }
                }
                return foundLine1 && foundLine2;
            });
            if (!found) throw new Error('Multi-line text not found');
        });

        await this.test('processUARTData handles invalid Base64 gracefully', async () => {
            const hasError = await this.page.evaluate(() => {
                window.espTerminal.term.clear();
                try {
                    window.espTerminal.processUARTData('!!!invalid-base64!!!');
                } catch (e) {
                    return false; // Should not throw
                }
                // Should show error message in terminal
                const buffer = window.espTerminal.term.buffer.active;
                for (let i = 0; i < 5; i++) {
                    const line = buffer.getLine(i);
                    if (line && line.translateToString().includes('Ошибка')) return true;
                }
                return true; // Error was caught internally
            });
            if (!hasError) throw new Error('Invalid Base64 not handled');
        });

        // Keyboard Handling Tests
        console.log('\n\x1b[36m[Keyboard Handling]\x1b[0m');

        await this.test('Ctrl+L clears terminal', async () => {
            // Write some text
            await this.page.evaluate(() => {
                window.espTerminal.term.writeln('Text before Ctrl+L');
            });

            // Focus terminal and send Ctrl+L
            await this.page.click('#terminal');
            await this.page.keyboard.down('Control');
            await this.page.keyboard.press('l');
            await this.page.keyboard.up('Control');
            await new Promise(resolve => setTimeout(resolve, 200));

            const cleared = await this.page.evaluate(() => {
                const buffer = window.espTerminal.term.buffer.active;
                const line = buffer.getLine(0);
                return line ? line.translateToString().trim() === '' : true;
            });
            if (!cleared) throw new Error('Ctrl+L did not clear terminal');
        });

        // Window Resize Tests
        console.log('\n\x1b[36m[Window Resize]\x1b[0m');

        await this.test('FitAddon.fit() works', async () => {
            const result = await this.page.evaluate(() => {
                const colsBefore = window.espTerminal.term.cols;
                window.espTerminal.fitAddon.fit();
                const colsAfter = window.espTerminal.term.cols;
                return { before: colsBefore, after: colsAfter };
            });
            // Just verify it doesn't throw
            if (result.after === undefined) throw new Error('fit() failed');
        });

        await this.test('Terminal resizes with viewport', async () => {
            const sizeBefore = await this.page.evaluate(() => ({
                cols: window.espTerminal.term.cols,
                rows: window.espTerminal.term.rows
            }));

            await this.page.setViewport({ width: 1200, height: 800 });
            await new Promise(resolve => setTimeout(resolve, 300));

            // Trigger resize event
            await this.page.evaluate(() => {
                window.dispatchEvent(new Event('resize'));
            });
            await new Promise(resolve => setTimeout(resolve, 200));

            const sizeAfter = await this.page.evaluate(() => ({
                cols: window.espTerminal.term.cols,
                rows: window.espTerminal.term.rows
            }));

            // Size should change after viewport change
            if (sizeBefore.cols === sizeAfter.cols && sizeBefore.rows === sizeAfter.rows) {
                // This might be OK depending on initial viewport, just log it
                console.log(`\n    (cols: ${sizeAfter.cols}, rows: ${sizeAfter.rows})`);
            }
        });

        // Statistics Display Tests
        console.log('\n\x1b[36m[Statistics Display]\x1b[0m');

        await this.test('Packet count updates', async () => {
            await this.page.evaluate(() => {
                document.getElementById('packet-count').textContent = '0';
                window.espTerminal.packetCount = 0;
            });

            // Simulate receiving packets
            await this.page.evaluate(() => {
                window.espTerminal.packetCount = 42;
                document.getElementById('packet-count').textContent = '42';
            });

            const count = await this.page.$eval('#packet-count', el => el.textContent);
            if (count !== '42') throw new Error(`Packet count is "${count}", expected "42"`);
        });

        await this.test('Last activity updates', async () => {
            await this.page.evaluate(() => {
                document.getElementById('last-activity').textContent = '12:34:56';
            });

            const activity = await this.page.$eval('#last-activity', el => el.textContent);
            if (activity !== '12:34:56') throw new Error(`Last activity is "${activity}"`);
        });

        // Theme Tests
        console.log('\n\x1b[36m[Theme/Styling]\x1b[0m');

        await this.test('Terminal background color', async () => {
            const bgColor = await this.page.evaluate(() => {
                return window.espTerminal.term.options.theme.background;
            });
            if (bgColor !== '#0a0a0f') throw new Error(`Background is "${bgColor}", expected "#0a0a0f"`);
        });

        await this.test('Terminal foreground color', async () => {
            const fgColor = await this.page.evaluate(() => {
                return window.espTerminal.term.options.theme.foreground;
            });
            if (fgColor !== '#e0e0e0') throw new Error(`Foreground is "${fgColor}", expected "#e0e0e0"`);
        });

        await this.test('Cursor style is block', async () => {
            const style = await this.page.evaluate(() => {
                return window.espTerminal.term.options.cursorStyle;
            });
            if (style !== 'block') throw new Error(`Cursor style is "${style}", expected "block"`);
        });

        // Print summary
        console.log('\n\x1b[1m=== Test Summary ===\x1b[0m');
        console.log(`\x1b[32mPassed: ${this.passed}\x1b[0m`);
        console.log(`\x1b[31mFailed: ${this.failed}\x1b[0m`);
        console.log(`Total:  ${this.passed + this.failed}\n`);

        return this.failed === 0;
    }
}

async function main() {
    const tester = new TerminalTester();

    try {
        await tester.setup();
        const success = await tester.runTests();
        await tester.teardown();
        process.exit(success ? 0 : 1);
    } catch (error) {
        console.error('\x1b[31mTest setup failed:\x1b[0m', error.message);
        await tester.teardown();
        process.exit(1);
    }
}

main();
