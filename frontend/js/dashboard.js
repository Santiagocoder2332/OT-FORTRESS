/* OT-FORTRESS - FUNCIONALIDAD DEL DASHBOARD INTERACTIVO */

// 0. Verificación de sesión
if (!localStorage.getItem('ot_token')) {
    window.location.href = 'login.html';
}

document.addEventListener("DOMContentLoaded", () => {
    // 1. Cargar y mostrar información del perfil del usuario autenticado
    const userJson = localStorage.getItem('ot_user');
    if (userJson) {
        try {
            const userObj = JSON.parse(userJson);
            
            // Actualización de la barra lateral (sidebar)
            const accountSection = document.querySelector('.account');
            if (accountSection) {
                const avatar = accountSection.querySelector('.avatar');
                if (avatar && userObj.fullName) {
                    const initials = userObj.fullName.split(' ').map(n => n[0]).join('').substring(0, 2).toUpperCase();
                    avatar.textContent = initials || 'US';
                }
                const nameStrong = accountSection.querySelector('strong');
                if (nameStrong && userObj.fullName) {
                    nameStrong.textContent = userObj.fullName;
                }
            }

            // Actualizaciones de la página de perfil (si estamos en profile.html)
            if (window.location.pathname.includes('profile.html')) {
                // Bloque de resumen del perfil
                const profileSummary = document.querySelector('.profile-summary');
                if (profileSummary) {
                    const summaryAvatar = profileSummary.querySelector('.avatar');
                    if (summaryAvatar && userObj.fullName) {
                        const initials = userObj.fullName.split(' ').map(n => n[0]).join('').substring(0, 2).toUpperCase();
                        summaryAvatar.textContent = initials || 'US';
                    }
                    const summaryName = profileSummary.querySelector('h2');
                    if (summaryName && userObj.fullName) {
                        summaryName.textContent = userObj.fullName;
                    }
                    const summaryMeta = profileSummary.querySelector('p');
                    if (summaryMeta && userObj.email) {
                        summaryMeta.textContent = `${userObj.email} · ${userObj.role || 'Operador de Seguridad'}`;
                    }
                    const badgesContainer = profileSummary.querySelector('.profile-summary__badges');
                    if (badgesContainer) {
                        badgesContainer.innerHTML = `
                            <span class="badge green">${userObj.role || 'Operador de Seguridad'}</span>
                            <span class="badge blue">MFA Activo</span>
                            <span class="badge green">TLS 1.3 Conectado</span>
                        `;
                    }
                }

                // Campos del formulario de perfil
                const profileForm = document.getElementById('profileForm');
                if (profileForm) {
                    const inputs = profileForm.querySelectorAll('input');
                    if (inputs.length >= 3) {
                        inputs[0].value = userObj.fullName || '';
                        inputs[1].value = userObj.email || '';
                        inputs[2].value = userObj.role || 'Operador de Seguridad';
                    }
                }
            }
        } catch (err) {
            console.error("Error al analizar la sesión de usuario:", err);
        }
    }

    // 1.2 Interceptar clics en enlaces de cierre de sesión
    const logoutLinks = document.querySelectorAll('a[href="login.html"]');
    logoutLinks.forEach(link => {
        link.addEventListener('click', (e) => {
            localStorage.removeItem('ot_token');
            localStorage.removeItem('ot_user');
        });
    });

    // 2. Sistema de Notificaciones Toast
    const toastContainer = document.getElementById("toastContainer") || createToastContainer();

    function createToastContainer() {
        const container = document.createElement("div");
        container.id = "toastContainer";
        container.className = "toast-container";
        document.body.appendChild(container);
        return container;
    }

    window.showToast = function(message) {
        const toast = document.createElement("div");
        toast.className = "toast-notification";
        toast.innerHTML = `<span>🛡️</span> <span>${message}</span>`;
        toastContainer.appendChild(toast);
        setTimeout(() => {
            toast.style.opacity = "0";
            toast.style.transform = "translateY(10px)";
            toast.style.transition = "all 0.3s ease";
            setTimeout(() => toast.remove(), 300);
        }, 3000);
    };

    // 3. Botón de Notificaciones y Popover
    const notificationButton = document.getElementById("notificationButton");
    const notificationPopover = document.getElementById("notificationPopover");
    const notifBadge = document.getElementById("notifBadge");

    if (notificationButton && notificationPopover) {
        notificationButton.addEventListener("click", (e) => {
            e.stopPropagation();
            notificationPopover.classList.toggle("active");
            if (notifBadge) {
                notifBadge.remove();
                showToast("Notificaciones marcadas como leídas");
            }
        });

        document.addEventListener("click", (e) => {
            if (!notificationPopover.contains(e.target) && e.target !== notificationButton) {
                notificationPopover.classList.remove("active");
            }
        });
    }

    // 4. Selectores de Período de Tiempo
    const periodButtons = document.querySelectorAll(".period-select");
    const periods = ["Última 1 h ⌄", "Últimas 24 h ⌄", "Últimos 7 días ⌄", "Últimos 30 días ⌄"];
    
    periodButtons.forEach((btn) => {
        btn.addEventListener("click", () => {
            let currentIndex = periods.findIndex(p => p === btn.textContent);
            let nextIndex = (currentIndex + 1) % periods.length;
            btn.textContent = periods[nextIndex];
            showToast(`Período de tiempo actualizado a ${periods[nextIndex].replace(" ⌄", "")}`);
        });
    });

    // 5. Sistema de Diálogos Modales Genéricos
    const modalBackdrop = document.getElementById("genericModal");
    const modalTitle = document.getElementById("modalTitle");
    const modalBody = document.getElementById("modalBody");
    const modalCloseBtn = document.getElementById("modalCloseBtn");
    const modalCancelBtn = document.getElementById("modalCancelBtn");
    const modalActionBtn = document.getElementById("modalActionBtn");

    window.openModal = function(title, contentHtml, actionBtnText = "Confirmar Acción", onAction = null) {
        if (!modalBackdrop) return;
        modalTitle.textContent = title;
        modalBody.innerHTML = contentHtml;
        if (modalActionBtn) {
            modalActionBtn.textContent = actionBtnText;
            modalActionBtn.onclick = () => {
                if (onAction) onAction();
                closeModal();
            };
        }
        modalBackdrop.classList.add("active");
    };

    window.closeModal = function() {
        if (modalBackdrop) modalBackdrop.classList.remove("active");
    };

    if (modalCloseBtn) modalCloseBtn.onclick = closeModal;
    if (modalCancelBtn) modalCancelBtn.onclick = closeModal;
    if (modalBackdrop) {
        modalBackdrop.addEventListener("click", (e) => {
            if (e.target === modalBackdrop) closeModal();
        });
    }

    // 6. Botones de Inspección y Detalles
    const inspectButtons = document.querySelectorAll(".inspect-btn, .outline-button");
    inspectButtons.forEach((btn) => {
        btn.addEventListener("click", (e) => {
            if (btn.id === "modalCancelBtn" || btn.classList.contains("modal-close")) return;
            const title = btn.dataset.title || "Información del elemento";
            const details = btn.dataset.details || "Elemento operativo activo. Verificado como seguro y protegido.";
            
            const content = `
                <div style="display:grid; gap:10px;">
                    <div style="padding:10px; background:#f4f9fc; border-radius:6px; border:1px solid #dceaf1;">
                        <strong>Estado:</strong> Conectado e Inspeccionado<br>
                        <strong>Detalles:</strong> ${details}
                    </div>
                    <p style="color:#6e8797; font-size:11px;">Todos los protocolos industriales y firmas de suma de comprobación coinciden con la línea base de seguridad.</p>
                </div>
            `;
            openModal(title, content, "Aceptar Elemento", () => {
                showToast("Acción confirmada correctamente");
            });
        });
    });

    // 7. Botones de Acción (+ Añadir Dispositivo, + Crear Regla, + Desplegar Sensor, Guardar Perfil)
    const addAssetBtn = document.getElementById("addAssetBtn");
    if (addAssetBtn) {
        addAssetBtn.addEventListener("click", () => {
            const formHtml = `
                <div style="display:grid; gap:12px;">
                    <div>
                        <label style="font-weight:700; font-size:11px;">Nombre del Dispositivo</label>
                        <input type="text" id="newAssetName" placeholder="Ej. Siemens PLC S7-1200" style="width:100%; height:34px; padding:0 8px; border:1px solid #d8e6ee; border-radius:6px;">
                    </div>
                    <div>
                        <label style="font-weight:700; font-size:11px;">Dirección IP</label>
                        <input type="text" id="newAssetIP" placeholder="Ej. 10.24.18.150" style="width:100%; height:34px; padding:0 8px; border:1px solid #d8e6ee; border-radius:6px;">
                    </div>
                    <div>
                        <label style="font-weight:700; font-size:11px;">Tipo / Protocolo</label>
                        <select id="newAssetType" style="width:100%; height:34px; border:1px solid #d8e6ee; border-radius:6px;">
                            <option>PLC · Modbus TCP</option>
                            <option>HMI · OPC UA</option>
                            <option>Gateway · EtherNet/IP</option>
                            <option>Servidor · WebSockets</option>
                        </select>
                    </div>
                </div>
            `;
            openModal("Añadir Nuevo Dispositivo OT", formHtml, "Añadir Dispositivo", () => {
                const name = document.getElementById("newAssetName")?.value || "Nuevo Dispositivo OT";
                const ip = document.getElementById("newAssetIP")?.value || "10.24.18.199";
                const type = document.getElementById("newAssetType")?.value || "PLC · Modbus TCP";
                
                // Añadir fila de dispositivo dinámicamente
                const table = document.querySelector(".data-table");
                if (table) {
                    const newRow = document.createElement("div");
                    newRow.className = "table-row asset-table";
                    newRow.innerHTML = `
                        <div><strong>${name}</strong><small>Entrada Manual</small></div>
                        <span>${ip}</span>
                        <span>${type}</span>
                        <span>Zona de Control</span>
                        <span class="status healthy">Protegido</span>
                        <button class="outline-button inspect-btn" data-title="${name}" data-details="IP: ${ip} | Tipo: ${type}">Detalles</button>
                    `;
                    table.appendChild(newRow);
                }
                showToast(`Dispositivo "${name}" añadido al inventario`);
            });
        });
    }

    const createRuleBtn = document.getElementById("createRuleBtn");
    if (createRuleBtn) {
        createRuleBtn.addEventListener("click", () => {
            const formHtml = `
                <div style="display:grid; gap:12px;">
                    <div>
                        <label style="font-weight:700; font-size:11px;">Nombre de la Regla</label>
                        <input type="text" placeholder="Ej. Bloquear función Modbus 0x05 no autorizada" style="width:100%; height:34px; padding:0 8px; border:1px solid #d8e6ee; border-radius:6px;">
                    </div>
                    <div>
                        <label style="font-weight:700; font-size:11px;">Acción</label>
                        <select style="width:100%; height:34px; border:1px solid #d8e6ee; border-radius:6px;">
                            <option>DROP (Bloquear y Alertar)</option>
                            <option>ALERT (Solo Registrar)</option>
                            <option>REJECT (Enviar TCP RST)</option>
                        </select>
                    </div>
                </div>
            `;
            openModal("Crear Regla de Protección IPS", formHtml, "Desplegar Regla", () => {
                showToast("Regla IPS desplegada en todos los sensores activos");
            });
        });
    }

    const deploySensorBtn = document.getElementById("deploySensorBtn");
    if (deploySensorBtn) {
        deploySensorBtn.addEventListener("click", () => {
            openModal("Desplegar Sensor Operativo", "<p>Iniciando el aprovisionamiento del nodo sensor en el segmento VLAN de OT...</p>", "Desplegar Nodo", () => {
                showToast("Aprovisionamiento de sensor iniciado");
            });
        });
    }

    const saveProfileBtn = document.getElementById("saveProfileBtn");
    if (saveProfileBtn) {
        saveProfileBtn.addEventListener("click", (e) => {
            e.preventDefault();
            const profileForm = document.getElementById('profileForm');
            if (profileForm) {
                const inputs = profileForm.querySelectorAll('input');
                if (inputs.length >= 3) {
                    try {
                        const userJson = localStorage.getItem('ot_user') || '{}';
                        const userObj = JSON.parse(userJson);
                        
                        userObj.fullName = inputs[0].value.trim();
                        userObj.email = inputs[1].value.trim();
                        userObj.role = inputs[2].value.trim();
                        
                        localStorage.setItem('ot_user', JSON.stringify(userObj));
                        showToast("Ajustes del perfil guardados correctamente");
                        
                        setTimeout(() => {
                            window.location.reload();
                        }, 500);
                        return;
                    } catch (err) {
                        console.error("Error al guardar los detalles del perfil:", err);
                    }
                }
            }
            showToast("Ajustes del perfil guardados correctamente");
        });
    }

    // 8. Filtrado de Búsqueda en Vivo
    const searchInput = document.getElementById("searchInput");
    if (searchInput) {
        searchInput.addEventListener("input", () => {
            const query = searchInput.value.toLowerCase().trim();
            const rows = document.querySelectorAll(".table-row, .timeline-event");
            rows.forEach((row) => {
                const text = row.textContent.toLowerCase();
                row.style.display = text.includes(query) ? "" : "none";
            });
        });
    }

    // 9. Toggles de Categoría de la Barra de Filtros
    const filterButtons = document.querySelectorAll(".filter-bar .filter");
    filterButtons.forEach((btn) => {
        btn.addEventListener("click", () => {
            filterButtons.forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            
            const category = btn.textContent.split(" ")[0].toLowerCase();
            const rows = document.querySelectorAll(".table-row, .timeline-event");
            
            rows.forEach((row) => {
                if (category === "all" || category === "todos" || category === "todo") {
                    row.style.display = "";
                } else {
                    const text = row.textContent.toLowerCase();
                    row.style.display = text.includes(category) ? "" : "none";
                }
            });
            showToast(`Filtro aplicado: ${btn.textContent}`);
        });
    });
});
