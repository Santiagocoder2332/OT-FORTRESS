const { app, BrowserWindow, dialog } = require('electron');
const path = require('path');
const http = require('http');

let mainWindow;

function waitForServer(retries = 30) {
    return new Promise((resolve, reject) => {
        const request = http.get('http://127.0.0.1:3000/api/health', (response) => {
            response.resume();
            response.statusCode === 200 ? resolve() : reject(new Error('El servidor local no está disponible.'));
        });
        request.on('error', () => {
            if (retries <= 0) return reject(new Error('No fue posible iniciar el servidor local.'));
            setTimeout(() => waitForServer(retries - 1).then(resolve, reject), 200);
        });
        request.setTimeout(1000, () => request.destroy());
    });
}

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 1440,
        height: 920,
        minWidth: 1024,
        minHeight: 700,
        backgroundColor: '#f4f9fc',
        icon: path.join(__dirname, 'build', 'icon.ico'),
        webPreferences: { contextIsolation: true, nodeIntegration: false }
    });
    mainWindow.removeMenu();
    mainWindow.loadURL('http://127.0.0.1:3000');
}

app.whenReady().then(async () => {
    process.env.OT_FORTRESS_DATA_DIR = path.join(app.getPath('userData'), 'data');
    require('./server');
    try {
        await waitForServer();
        createWindow();
    } catch (error) {
        dialog.showErrorBox('OT-FORTRESS', error.message);
        app.quit();
    }
});

app.on('window-all-closed', () => app.quit());
