// --- CONFIGURATION ---
const firebaseConfig = {
    databaseURL: "https://smart-parking-system-d6e6c-default-rtdb.asia-southeast1.firebasedatabase.app",
    projectId: "smart-parking-system-d6e6c"
};

firebase.initializeApp(firebaseConfig);
const db = firebase.database();
const fs = firebase.firestore();

// Global Charts
let revChart = null;
let pkChart = null;
let durChart = null; // เพิ่มใหม่
let occChart = null; // เพิ่มใหม่

// --- INITIAL CHECK (เช็คสถานะล็อกอินทันทีที่เปิดหน้า) ---
document.addEventListener("DOMContentLoaded", () => {
    const isAdmin = localStorage.getItem("adminSession");
    if (isAdmin === "active") {
        showAdminView();
    }
});

// --- NAVIGATION LOGIC ---
function loginAdmin() {
    const pass = prompt("กรุณากรอกรหัสผ่านเจ้าของลานจอด:");
    if (pass === "123") {
        localStorage.setItem("adminSession", "active");
        showAdminView();
    } else if (pass !== null) {
        alert("รหัสผ่านไม่ถูกต้อง!");
    }
}

function logoutAdmin() {
    localStorage.removeItem("adminSession");
    location.reload();
}

function showAdminView() {
    document.getElementById('user-view').style.display = 'none';
    document.getElementById('admin-section').style.display = 'block';
    document.getElementById('nav-auth-buttons').style.display = 'none';
    document.getElementById('admin-logout-btn').style.display = 'block';
    loadAdminData();
}

// --- REAL-TIME SLOTS (FOR USERS) ---
db.ref('parking_status').on('value', (snap) => {
    const data = snap.val();
    const container = document.getElementById('slots-container');
    if (!data || !container) return;

    container.innerHTML = '';
    for (let i = 0; i < 6; i++) {
        const isOccupied = data[`slot_${i}`];
        container.innerHTML += `
            <div class="col-6 col-md-4 col-lg-2">
                <div class="parking-slot ${isOccupied ? 'bg-occupied' : 'bg-vacant'}">
                    <i class="bi ${isOccupied ? 'bi-car-front-fill' : 'bi-p-square'} fs-1 mb-2"></i>
                    <span class="fw-bold">ช่อง ${i + 1}</span>
                    <small class="opacity-75">${isOccupied ? 'ไม่ว่าง' : 'ว่าง'}</small>
                </div>
            </div>`;
    }
});

// --- ADMIN DATA & DASHBOARD (FULL VERSION - USE TOTAL SECONDS) ---
function loadAdminData() {
    const now = new Date();
    const todayStr = `${String(now.getDate()).padStart(2, '0')}-${String(now.getMonth() + 1).padStart(2, '0')}-${now.getFullYear()}`;
    
    const dateLabel = document.getElementById('current-date-label');
    if (dateLabel) dateLabel.innerText = "ข้อมูล ณ วันที่: " + todayStr + " " + now.toLocaleTimeString('th-TH');

    fs.collection("ParkingLogs").orderBy("created_at", "desc").onSnapshot((snap) => {
        const activeBody = document.getElementById('active-table-body');
        const historyBody = document.getElementById('history-table-body');
        
        if (activeBody) activeBody.innerHTML = '';
        if (historyBody) historyBody.innerHTML = '';
        
        let activeCount = 0;
        let todayRev = 0;
        let todayTrans = 0;
        let revenue7Days = {};
        let peakData = new Array(24).fill(0);
        let durationCounts = [0, 0, 0, 0]; // [0-5น., 5-15น., 15-30น., 30น.+]
        window.dailyVehicleCount = {}; 

        snap.forEach((doc) => {
            const d = doc.data();
            const status = (d.status || "").toLowerCase().trim();

            if (status === "parked" || status === "active") {
                activeCount++;
                if (activeBody) {
                    activeBody.innerHTML += `
                        <tr>
                            <td class="ps-4"><span class="fw-bold text-primary">${d.plate}</span></td>
                            <td>
                                <small class="text-muted">${d.entry_date}</small><br>
                                <i class="bi bi-clock-fill text-warning me-1"></i><b>${d.entry_time}</b>
                            </td>
                            <td><img src="${d.photo_url_in}" class="img-thumb" onclick="window.open(this.src)"></td>
                            <td class="text-center"><span class="badge bg-warning text-dark badge-custom">จอดอยู่</span></td>
                        </tr>`;
                }
            } 
            else if (status === "completed") {
                todayTrans++; 
                
                if (historyBody) {
                    historyBody.innerHTML += `
                        <tr>
                            <td class="ps-4"><b>${d.plate}</b></td>
                            <td><small class="text-muted">${d.entry_date}</small><br><b>${d.entry_time}</b></td>
                            <td><small class="text-muted">${d.exit_date || d.entry_date}</small><br><b class="text-danger">${d.exit_time}</b></td>
                            <td class="text-center">
                                <img src="${d.photo_url_in}" class="img-thumb me-1" title="รูปเข้า" onclick="window.open(this.src)">
                                <img src="${d.photo_url_out}" class="img-thumb" title="รูปออก" onclick="window.open(this.src)">
                            </td>
                            <td class="text-center"><span class="badge bg-light text-dark border">${d.duration_summary || '-'}</span></td>
                            <td class="text-end pe-4 fw-bold text-success">${(d.fee || 0).toLocaleString()} ฿</td>
                        </tr>`;
                }

                // --- LOGIC การคำนวณ CHART ---

                const dateKey = d.exit_date;
                if (dateKey) {
                    revenue7Days[dateKey] = (revenue7Days[dateKey] || 0) + (d.fee || 0);
                    window.dailyVehicleCount[dateKey] = (window.dailyVehicleCount[dateKey] || 0) + 1;
                    if (dateKey === todayStr) todayRev += (d.fee || 0);
                }

                if (d.entry_time) {
                    const hr = parseInt(d.entry_time.split(':')[0]);
                    if (!isNaN(hr)) peakData[hr]++;
                }

                // ✅ ใช้ total_seconds ตามที่คุณแนะนำ (แม่นยำที่สุด)
                // แปลงวินาทีเป็นนาที: 36399 / 60 = 606.65 นาที
                let totalMins = d.total_seconds ? (d.total_seconds / 60) : 0;

                // จัดกลุ่มนาทีจำลอง
                if (totalMins > 0 && totalMins <= 5) {
                    durationCounts[0]++;
                } else if (totalMins > 5 && totalMins <= 15) {
                    durationCounts[1]++;
                } else if (totalMins > 15 && totalMins <= 30) {
                    durationCounts[2]++;
                } else if (totalMins > 30) {
                    durationCounts[3]++; // รถที่จอด 36399 วินาที (10 ชม.) จะมาลงที่นี่แน่นอน
                }
            }
        });

        if (document.getElementById('today-revenue')) 
            document.getElementById('today-revenue').innerText = todayRev.toLocaleString() + " ฿";
        if (document.getElementById('admin-occupied')) 
            document.getElementById('admin-occupied').innerText = activeCount;
        if (document.getElementById('active-badge-count')) 
            document.getElementById('active-badge-count').innerText = activeCount + " คัน";
        if (document.getElementById('total-transactions')) 
            document.getElementById('total-transactions').innerText = todayTrans;

        updateCharts(revenue7Days, peakData, durationCounts);
    });
}

function updateCharts(revData, peakData, durDataRaw) {
    const labels = Object.keys(revData).sort().slice(-7);
    const values = labels.map(k => revData[k]);

    // 1. กราฟรายได้ (อันเก่าอย่ายุ่ง)
    if (revChart) revChart.destroy();
    revChart = new Chart(document.getElementById('revenueChart'), {
        type: 'bar',
        data: {
            labels: labels,
            datasets: [{ label: 'รายได้', data: values, backgroundColor: '#4361ee', borderRadius: 8 }]
        },
        options: { 
            plugins: { legend: { display: false } }, 
            scales: { 
                y: { 
                    beginAtZero: true,
                    ticks: {
                        // แสดงเครื่องหมาย ฿ และทำให้เป็นเลขจำนวนเต็ม
                        callback: function(value) { return value.toLocaleString() + ' ฿'; }
                    }
                } 
            } 
        }
    });

    // 2. กราฟจำนวนรถเข้า (อันเก่าอย่ายุ่ง)
    if (pkChart) pkChart.destroy();
    pkChart = new Chart(document.getElementById('peakChart'), {
        type: 'line',
        data: {
            labels: Array.from({length: 24}, (_, i) => i + ":00"),
            datasets: [{ 
                label: 'จำนวนรถเข้า', 
                data: peakData, 
                borderColor: '#4cc9f0', 
                fill: true, 
                backgroundColor: 'rgba(76, 201, 240, 0.1)', 
                tension: 0.4 
            }]
        },
        options: {
            plugins: { legend: { display: false } },
            scales: {
                y: {
                    beginAtZero: true,
                    ticks: {
                        stepSize: 1,      // บังคับกระโดดทีละ 1 หน่วย
                        precision: 0,     // ไม่แสดงทศนิยม
                        callback: function(value) {
                            if (value % 1 === 0) return value; // คืนค่าเฉพาะเลขเต็ม
                        }
                    }
                }
            }
        }
    });

    // --- เพิ่มใหม่ 3. กราฟสัดส่วนระยะเวลาการจอด (นาทีจำลอง) ---
    if (durChart) durChart.destroy();
    durChart = new Chart(document.getElementById('durationChart'), {
        type: 'doughnut',
        data: {
            labels: ['0-5 นาที', '5-15 นาที', '15-30 นาที', '30 นาที+'],
            datasets: [{
                data: durDataRaw, // ข้อมูลที่ส่งมาจาก loadAdminData
                backgroundColor: ['#4cc9f0', '#4361ee', '#3f37c9', '#f72585'],
                borderWidth: 2,
                hoverOffset: 10
            }]
        },
        options: {
            maintainAspectRatio: false,
            plugins: {
                legend: { position: 'bottom', labels: { padding: 20, usePointStyle: true } }
            }
        }
    });

    // --- เพิ่มใหม่ 4. กราฟจำนวนรถสะสม 7 วันล่าสุด (เลขจำนวนเต็ม) ---
    const vehicleCounts = labels.map(k => window.dailyVehicleCount[k] || 0); 
    if (occChart) occChart.destroy();
    occChart = new Chart(document.getElementById('occupancyChart'), {
        type: 'bar',
        data: {
            labels: labels,
            datasets: [{
                label: 'จำนวนรถ (คัน)',
                data: vehicleCounts,
                backgroundColor: '#f39c12',
                borderRadius: 8
            }]
        },
        options: {
            maintainAspectRatio: false,
            plugins: { legend: { display: false } },
            scales: {
                y: {
                    beginAtZero: true,
                    ticks: {
                        stepSize: 1,
                        precision: 0,
                        callback: function(value) { return value + ' คัน'; }
                    }
                }
            }
        }
    });
}

// --- USER SEARCH LOGIC ---
function searchVehicle() {
    const plate = document.getElementById('search-plate').value.trim();
    const resultDiv = document.getElementById('search-result');
    if (!plate) return alert("กรุณาระบุเลขทะเบียน");

    resultDiv.innerHTML = '<div class="text-center"><div class="spinner-border text-primary"></div></div>';

    fs.collection("ParkingLogs").where("plate", "==", plate).orderBy("created_at", "desc").limit(1).get().then((snap) => {
        if (snap.empty) {
            resultDiv.innerHTML = `<div class="alert alert-light border shadow-sm text-center">ไม่พบข้อมูลทะเบียน <b>${plate}</b></div>`;
            return;
        }
        snap.forEach(doc => {
            const d = doc.data();
            const isParked = (d.status === 'parked' || d.status === 'active');
            let fee = d.fee || 0;
            let duration = d.duration_summary || '-';

            if (isParked) {
                const diffMs = (Date.now() - d.created_at.toMillis()) + (7 * 3600000); // Correct for GMT+7
                const mins = Math.max(1, Math.ceil(diffMs / 60000));
                fee = mins * 10;
                duration = `${Math.floor(mins/60)} ชม. ${mins%60} นาที`;
            }

            resultDiv.innerHTML = `
                <div class="card border-0 shadow-lg p-4" style="border-radius:20px;">
                    <div class="row align-items-center">
                        <div class="col-md-4"><img src="${d.photo_url_in}" class="img-fluid rounded-4"></div>
                        <div class="col-md-8">
                            <span class="badge ${isParked ? 'bg-warning' : 'bg-success'} mb-2">${isParked ? 'กำลังจอด' : 'ออกแล้ว'}</span>
                            <h3 class="fw-bold">${d.plate}</h3>
                            <p class="mb-1 text-muted">เวลาเข้า: ${d.entry_date} ${d.entry_time}</p>
                            <p class="mb-3 text-muted">เวลาจอดรวม: ${duration}</p>
                            <h2 class="text-primary fw-bold">${fee.toLocaleString()} บาท</h2>
                        </div>
                    </div>
                </div>`;
        });
    });
}