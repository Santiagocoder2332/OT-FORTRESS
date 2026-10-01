/* OT-FORTRESS - LOGIN JAVASCRIPT CON NAVEGACIÓN DESLIZANTE */

document.addEventListener("DOMContentLoaded", () => {
    const glassCard = document.querySelector(".glass-card");
    const loginForm = document.getElementById("loginForm");
    const usernameInput = document.getElementById("username");
    const passwordInput = document.getElementById("password");
    const loginButton = document.getElementById("loginButton");
    const loginMessage = document.getElementById("loginMessage");
    const signupLink = document.querySelector(".signup-footer a");

    // Detección de animación al entrar desde Registro
    const urlParams = new URLSearchParams(window.location.search);
    if (urlParams.get("slide") === "from-right") {
        glassCard?.classList.add("slide-in-left");
    }

    // Transición deslizante al hacer clic en "Sign up" / Registrarse (Desliza a la izquierda)
    signupLink?.addEventListener("click", (e) => {
        e.preventDefault();
        const targetUrl = signupLink.getAttribute("href");
        if (glassCard) {
            glassCard.classList.remove("slide-in-left", "slide-in-right");
            glassCard.classList.add("slide-out-left");
            setTimeout(() => {
                window.location.href = targetUrl + "?slide=from-left";
            }, 380);
        } else {
            window.location.href = targetUrl;
        }
    });

    // Formulario de inicio de sesión
    loginForm?.addEventListener("submit", async (e) => {
        e.preventDefault();
        const email = usernameInput?.value.trim();
        const password = passwordInput?.value.trim();

        if (!email || !password) {
            return showError("Por favor ingresa tu usuario y contraseña.");
        }

        loginButton.disabled = true;
        if (loginButton) loginButton.style.opacity = "0.7";
        showSuccess("Verificando credenciales...");

        try {
            // La API se sirve desde el mismo origen que la interfaz: evita fallos de CORS/red.
            const response = await fetch("/api/auth/login", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ email, password })
            });

            const data = await response.json();

            if (!response.ok) {
                throw new Error(data.error || "Error al iniciar sesión.");
            }

            // Guardar token JWT y datos de usuario
            localStorage.setItem("ot_token", data.token);
            localStorage.setItem("ot_user", JSON.stringify(data.user));

            showSuccess("Acceso autorizado. Redirigiendo a la consola...");
            setTimeout(() => {
                window.location.href = "dashboard.html";
            }, 600);
        } catch (err) {
            showError(err.message || "Error al conectar con el servidor.");
        } finally {
            if (loginButton) {
                loginButton.disabled = false;
                loginButton.style.opacity = "1";
            }
        }
    });

    function showError(msg) {
        if (loginMessage) {
            loginMessage.textContent = msg;
            loginMessage.className = "login-message error";
        }
    }

    function showSuccess(msg) {
        if (loginMessage) {
            loginMessage.textContent = msg;
            loginMessage.className = "login-message success";
        }
    }
});
