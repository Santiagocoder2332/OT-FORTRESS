const express = require('express');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const initSqlJs = require('sql.js');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = Number(process.env.PORT) || 3000;
const JWT_SECRET = process.env.JWT_SECRET || 'ot-fortress-local-session-2026';
const dataDirectory = process.env.OT_FORTRESS_DATA_DIR || path.join(__dirname, 'data');
const databasePath = path.join(dataDirectory, 'ot_fortress.db');
const frontendPath = (() => {
    // En producción, electron-builder deja los recursos junto al ejecutable.
    const packagedPath = path.join(process.resourcesPath || '', 'frontend');
    if (fs.existsSync(packagedPath)) return packagedPath;
    // Permite usar una copia local del frontend si se añade en otro paquete.
    const internalPath = path.join(__dirname, 'frontend');
    if (fs.existsSync(internalPath)) return internalPath;
    // En desarrollo, el frontend vive un nivel arriba del backend.
    return path.join(__dirname, '..', 'frontend');
})();


// SQLite es la única base de datos. La contraseña se conserva exclusivamente como hash bcrypt.
const ACCOUNT = {
    fullName: 'Santiago Amezquita Alfonso',
    email: 'santiagoamezquita',
    passwordHash: '$2a$12$dt/AEXkpx1ZSNNtL/.5L0esUNhJLbzqkc4JrAK206TrscfYlyL1ve',
    role: 'Security Operator'
};

let db;

function persistDatabase() {
    fs.writeFileSync(databasePath, Buffer.from(db.export()));
}

function userFromRow(row) {
    return { id: row[0], fullName: row[1], email: row[2], role: row[3] };
}

function findUserByEmail(email) {
    const result = db.exec(
        'SELECT id, full_name, email, role, password_hash FROM users WHERE email = ?',
        [email]
    );
    return result[0]?.values[0] || null;
}

async function initializeDatabase() {
    fs.mkdirSync(dataDirectory, { recursive: true });
    const SQL = await initSqlJs();
    db = fs.existsSync(databasePath)
        ? new SQL.Database(fs.readFileSync(databasePath))
        : new SQL.Database();

    db.run(`CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        full_name TEXT NOT NULL,
        email TEXT NOT NULL UNIQUE,
        password_hash TEXT NOT NULL,
        role TEXT NOT NULL
    )`);

    // Esta consola usa una única cuenta administrativa; no hay registro público.
    db.run('DELETE FROM users');
    db.run(
        'INSERT INTO users (full_name, email, password_hash, role) VALUES (?, ?, ?, ?)',
        [ACCOUNT.fullName, ACCOUNT.email, ACCOUNT.passwordHash, ACCOUNT.role]
    );
    persistDatabase();
}

function requireSession(req, res, next) {
    const token = req.headers.authorization?.replace(/^Bearer\s+/i, '');
    if (!token) return res.status(401).json({ error: 'Sesión no proporcionada.' });
    try {
        req.session = jwt.verify(token, JWT_SECRET);
        next();
    } catch {
        res.status(401).json({ error: 'Sesión inválida o expirada.' });
    }
}

app.use(express.json({ limit: '16kb' }));

app.get('/api/health', (_req, res) => {
    res.json({ status: 'en_linea', database: 'SQLite', accountMode: 'single-user' });
});

app.post('/api/auth/login', (req, res) => {
    const email = String(req.body?.email || '').trim().toLowerCase();
    const password = String(req.body?.password || '');
    const row = findUserByEmail(email);

    if (!row || !bcrypt.compareSync(password, row[4])) {
        return res.status(401).json({ error: 'Correo electrónico o contraseña incorrectos.' });
    }

    const user = userFromRow(row);
    const token = jwt.sign({ id: user.id, email: user.email }, JWT_SECRET, { expiresIn: '8h' });
    return res.json({ message: 'Autenticación exitosa', token, user });
});

app.get('/api/auth/me', requireSession, (req, res) => {
    const row = findUserByEmail(req.session.email);
    if (!row) return res.status(401).json({ error: 'Sesión inválida.' });
    return res.json({ user: userFromRow(row) });
});

app.get('/', (_req, res) => res.redirect('/html/login.html'));
app.use(express.static(frontendPath));
app.use((err, _req, res, _next) => {
    if (err instanceof SyntaxError) return res.status(400).json({ error: 'JSON inválido.' });
    console.error(err);
    return res.status(500).json({ error: 'Error interno del servidor.' });
});

initializeDatabase()
    .then(() => app.listen(PORT, () => {
        console.log(`OT-FORTRESS listo en http://localhost:${PORT}`);
        console.log(`Base de datos única: SQLite (${databasePath})`);
    }))
    .catch((error) => {
        console.error('No fue posible iniciar SQLite:', error);
        process.exit(1);
    });
