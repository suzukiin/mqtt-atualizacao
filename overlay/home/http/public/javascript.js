document.addEventListener("DOMContentLoaded", function () {

    async function fetchNavbarInfo() {

        const response = await fetch("/cgi-bin/get-info-navbar");
        const res = await response.json();

        if (res.status == "Success") {
            document.getElementById("location-config-nav").textContent = res.data.location || "--";
            document.getElementById("client-config-nav").textContent = res.data.client || "--";
            document.getElementById("wan-ip-config-nav").textContent = res.data.wanIp || "--";
            document.getElementById("lan-ip-config-nav").textContent = res.data.lanIp || "--";
            document.getElementById("id-config-nav").textContent = res.data.deviceId || "--";
            document.getElementById("version-config-nav").textContent = res.data.version || "--";
        } else {
            window.alert("Erro: " + res.msg);
        }

    }

    async function fetchUptime() {

        const response = await fetch("/cgi-bin/get-uptime");
        const res = await response.json();

        if (res.status == "Success") {
            var uptimeElement = document.getElementById("uptime");
            var dias = res.data.dias;
            var horas = res.data.horas;
            var minutos = res.data.minutos;
            uptimeElement.textContent = `${dias}d ${horas}h ${minutos}m`;
        } else {
            window.alert("Erro: " + res.msg);
        }
    }

    async function fetchTraffic() {

        const resopnse = await fetch("/cgi-bin/get-traffic");
        const res = await resopnse.json();

        if (res.status == "Success") {
            document.getElementById("rx-traffic").textContent = res.data.rx_mb + " MB";
            document.getElementById("tx-traffic").textContent = res.data.tx_mb + " MB";
        } else {
            window.alert("Erro: " + res.msg);
        }
    }

    async function fetchCpuTemperature() {
        const response = await fetch("/cgi-bin/get-temp-cpu");
        const res = await response.json();

        const tempElement = document.getElementById("cpu-temp");
        const statusElement = document.getElementById("cpu-temp-status");

        if (res.status == "Success") {
            const tempCelsius = Number(res.data.celsius);
            tempElement.textContent = Number.isFinite(tempCelsius) ? `${tempCelsius.toFixed(1)} °C` : "--";

            if (Number.isFinite(tempCelsius)) {
                if (tempCelsius >= 80) {
                    statusElement.textContent = "HIGH";
                    statusElement.className = "text-danger font-mono";
                } else if (tempCelsius >= 65) {
                    statusElement.textContent = "WARM";
                    statusElement.className = "text-warning font-mono";
                } else {
                    statusElement.textContent = "NORMAL";
                    statusElement.className = "text-success font-mono";
                }
            } else {
                statusElement.textContent = "--";
                statusElement.className = "text-warning font-mono";
            }
        } else {
            tempElement.textContent = "--";
            statusElement.textContent = "ERROR";
            statusElement.className = "text-danger font-mono";
        }
    }

    async function fetchLogs() {
        const response = await fetch("/cgi-bin/get-log-watchdog");
        const res = await response.json();

        if (res.status == "Success") {
            const logsBody = document.getElementById("logs-body");
            logsBody.innerHTML = "";

            res.logs.forEach(logLine => {
                const parts = logLine.split(" - ");
                const timestamp = parts[0] || "--";
                const message = parts[1] || logLine;

                const tr = document.createElement("tr");
                tr.innerHTML = `
                    <td class="font-mono small">${timestamp}</td>
                    <td><span class="badge bg-info bg-opacity-10 text-info border border-info border-opacity-25 small">INFO</span></td>
                    <td class="text-muted small">WATCHDOG</td>
                    <td class="text-light small">${message}</td>
                `;
                logsBody.appendChild(tr);
            });
        } else {
            window.alert("Erro: " + res.msg);

        }
    }

    async function fetchTelemetry() {
        try {
            const response = await fetch("public/telemetry_data.json?nocache=" + new Date().getTime());
            if (!response.ok) throw new Error("Erro na requisição");
            
            const data = await response.json();
            const container = document.getElementById("telemetry-container");
            
            if (!data.equipamentos) {
                container.innerHTML = `<div class="col-12 text-center text-muted">Nenhum dado de telemetria disponível.</div>`;
                return;
            }

            let html = "";
            data.equipamentos.forEach(eq => {
                let sensorsHtml = "";
                
                if (eq.OIDS && eq.OIDS.length > 0) {
                    eq.OIDS.forEach(oid => {
                        let valueDisplay = "--";
                        let statusClass = "text-light";

                        if (oid.last_error) {
                            valueDisplay = `<span class="text-danger" title="Erro de leitura"><i class="bi bi-exclamation-triangle"></i></span>`;
                        } else if (oid.last_value !== undefined) {
                            // Se tiver status (enum), prioriza exibir ele se for string
                            if (oid.last_status) {
                                valueDisplay = oid.last_status;
                                const valLower = String(valueDisplay).toLowerCase();
                                if (valLower === "ok" || valLower === "locked" || valLower === "active") statusClass = "text-success";
                                else if (valLower.includes("warning")) statusClass = "text-warning";
                                else if (valLower.includes("error") || valLower.includes("alarm") || valLower.includes("fail")) statusClass = "text-danger";
                            } else {
                                valueDisplay = oid.last_value;

                                // Aplica mascara de 0.001 para potencia direta
                                const oidName = (oid.nome || oid.topico || "").toLowerCase();
                                if (oidName.includes("potencia direta") || oidName.includes("potência direta")) {
                                    if (typeof valueDisplay === 'number') {
                                        valueDisplay = valueDisplay * 0.001;
                                    }
                                }

                                // Se for número e tiver máscara decimal, talvez formatar? Por enquanto deixa raw.
                                if (typeof valueDisplay === 'number' && !Number.isInteger(valueDisplay)) {
                                    valueDisplay = valueDisplay.toFixed(2);
                                }
                            }

                            if (oid.unidade) {
                                valueDisplay += ` <small class="text-muted ml-1">${oid.unidade}</small>`;
                            }
                        }

                        sensorsHtml += `
                            <div class="d-flex justify-content-between align-items-center border-bottom border-secondary border-opacity-10 py-2">
                                <span class="text-secondary small text-uppercase" style="font-size: 0.75rem">${oid.nome || oid.topico}</span>
                                <span class="font-mono fw-bold ${statusClass} small">${valueDisplay}</span>
                            </div>
                        `;
                    });
                }

                html += `
                    <div class="col-md-6 col-xl-4">
                        <div class="dash-card h-100">
                            <div class="d-flex justify-content-between align-items-center mb-3">
                                <div>
                                    <h6 class="text-white mb-0 text-uppercase">${eq.modelo || "Equipamento"}</h6>
                                    <small class="text-muted font-mono" style="font-size: 0.70rem">SN: ${eq.numero_de_serie || "--"}</small>
                                </div>
                                <div class="p-2 bg-dark border border-secondary border-opacity-25 rounded-2">
                                     <i class="bi bi-hdd-network text-secondary"></i>
                                </div>
                            </div>
                            <div class="mt-2 text-white">
                                ${sensorsHtml}
                            </div>
                            <div class="mt-3 text-end pt-2 border-top border-secondary border-opacity-10">
                                <small class="text-muted fst-italic" style="font-size: 0.65rem">
                                    <i class="bi bi-geo-alt me-1"></i>${eq.site_id} &bull; ${eq.ip}
                                </small>
                            </div>
                        </div>
                    </div>
                `;
            });

            container.innerHTML = html;

        } catch (error) {
            console.error("Erro ao buscar telemetria:", error);
        }
    }

    async function fetchStatusVpn() {

        const response = await fetch("/cgi-bin/get-status-vpn");
        const res = await response.json();

        var connectionBagdeElement = document.getElementById("connection-badge");
        var connectionDotElement = document.getElementById("connection-dot");
        var connectionTextElement = document.getElementById("connection-text");

        if (res.status == "Success") {

            connectionBagdeElement.classList.replace('bg-danger', 'bg-success');
            connectionBagdeElement.classList.replace('text-danger', 'text-success');
            connectionBagdeElement.classList.replace('border-danger', 'border-success');

            connectionDotElement.classList.replace('status-offline', 'status-online');
            
            connectionTextElement.textContent = "ONLINE";
        } else {

            connectionBagdeElement.classList.replace('bg-success', 'bg-danger');
            connectionBagdeElement.classList.replace('text-success', 'text-danger');
            connectionBagdeElement.classList.replace('border-success', 'border-danger');

            connectionDotElement.classList.replace('status-online', 'status-offline');

            connectionTextElement.textContent = "OFFLINE";
        }
    }

    async function fetchRssi() {
        try {
            const response = await fetch("/cgi-bin/get-rssi");
            const res = await response.json();

            const rssiElement = document.getElementById("modem-rssi");
            const statusElement = document.getElementById("modem-rssi-status");

            if (res.status == "Success") {
                const rssi = res.data.rssi;
                rssiElement.textContent = rssi;

                if (rssi >= 20 && rssi <= 31) {
                    statusElement.textContent = "EXCELENTE";
                    statusElement.className = "text-success font-mono";
                } else if (rssi >= 15 && rssi < 20) {
                    statusElement.textContent = "BOM";
                    statusElement.className = "text-info font-mono";
                } else if (rssi >= 10 && rssi < 15) {
                    statusElement.textContent = "REGULAR";
                    statusElement.className = "text-warning font-mono";
                } else if (rssi > 0 && rssi < 10) {
                    statusElement.textContent = "FRACO";
                    statusElement.className = "text-danger font-mono";
                } else if (rssi == 99) {
                    statusElement.textContent = "SEM SINAL";
                    statusElement.className = "text-danger font-mono";
                    rssiElement.textContent = "--";
                } else {
                    statusElement.textContent = "DESCONHECIDO";
                    statusElement.className = "text-muted font-mono";
                }
            } else {
                rssiElement.textContent = "--";
                statusElement.textContent = "ERRO";
                statusElement.className = "text-danger font-mono";
            }
        } catch (error) {
            console.error("Erro ao buscar RSSI:", error);
            document.getElementById("modem-rssi").textContent = "--";
            document.getElementById("modem-rssi-status").textContent = "ERRO";
            document.getElementById("modem-rssi-status").className = "text-danger font-mono";
        }
    }

    fetchStatusVpn();
    fetchLogs();
    fetchNavbarInfo();
    fetchUptime();
    fetchTraffic();
    fetchCpuTemperature();
    fetchTelemetry();
    fetchRssi();

    setInterval(fetchUptime, 60000);
    setInterval(fetchCpuTemperature, 10000);
    setInterval(fetchTelemetry, 10000);
    setInterval(fetchRssi, 10000);
});