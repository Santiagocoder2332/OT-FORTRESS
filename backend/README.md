# Backend OT-FORTRESS

Este proyecto usa una única base de datos: **SQLite**, almacenada en `data/ot_fortress.db`.

El servicio entrega la interfaz y la API desde el mismo proceso para evitar errores de CORS o conexiones a una API que no se haya iniciado.

## Ejecutar

```powershell
cd backend
npm start
```

Abra `http://localhost:3000` en el navegador. No abra los archivos HTML directamente.

La aplicación está configurada en modo de cuenta única. Al iniciar, SQLite conserva únicamente la cuenta autorizada y no expone registro público.
