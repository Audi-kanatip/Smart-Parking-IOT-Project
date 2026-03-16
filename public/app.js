// --- CONFIGURATION ---
const firebaseConfig = {
    databaseURL: "https://smart-parking-system-d6e6c-default-rtdb.asia-southeast1.firebasedatabase.app",
    projectId: "smart-parking-system-d6e6c"
};

// --- 🛑 ประกาศตัวแปร Chart ไว้ตรงนี้ (Global Scope) ---
let revChart = null;
let pkChart = null;
let durChart = null;
let occupancyChart = null; // เพิ่มให้ครบตาม HTML ของคุณ
firebase.initializeApp(firebaseConfig);
const db = firebase.database();
const fs = firebase.firestore();

// --- LOGIN LOGIC ---
function loginAdmin() {
    const pass = prompt("Owner Password:");
    if (pass === "123") {
        const adminSec = document.getElementById('admin-section');
        if (adminSec) {
            adminSec.style.display = 'block';
            loadAdminData();
            window.scrollTo({ top: adminSec.offsetTop, behavior: 'smooth' });
        }
    } else if (pass !== null) {
        alert("รหัสผ่านไม่ถูกต้อง");
    }
}

// --- USER ROLE: REALTIME SLOTS (RTDB) ---
db.ref('parking_status').on('value', (snap) => {
    const data = snap.val();
    if (!data) return;
    const freeCount = 6 - (data.total_occupied || 0);
    const freeElem = document.getElementById('free-count');
    if (freeElem) freeElem.innerText = freeCount;

    const container = document.getElementById('slots-container');
    if (container) {
        container.innerHTML = '';
        for (let i = 0; i < 6; i++) {
            const occ = data[`slot_${i}`];
            container.innerHTML += `
                <div class="col-4">
                    <div class="parking-slot ${occ ? 'bg-occupied' : 'bg-vacant'} text-center p-3 mb-2 shadow-sm" style="border-radius:10px; color:white; min-height:80px;">
                        <i class="bi ${occ ? 'bi-car-front-fill' : 'bi-unlock-fill'} fs-4"></i><br>
                        <b>ช่อง ${i + 1}</b>
                    </div>
                </div>`;
        }
    }
});

// --- ADMIN ROLE: LOAD DATA & REFRESH TABLES ---
function loadAdminData() {
    const now = new Date();
    const todayStr = `${String(now.getDate()).padStart(2, '0')}-${String(now.getMonth() + 1).padStart(2, '0')}-${now.getFullYear()}`;
    
    // อัปเดตวันที่ปัจจุบันที่หน้า Dashboard
    const dateLabel = document.getElementById('current-date-label');
    if (dateLabel) dateLabel.innerText = "ข้อมูล ณ วันที่ " + todayStr;

    fs.collection("ParkingLogs").orderBy("created_at", "desc").onSnapshot((snap) => {
        const activeBody = document.getElementById('active-table-body');
        const historyBody = document.getElementById('history-table-body');
        
        if (activeBody) activeBody.innerHTML = '';
        if (historyBody) historyBody.innerHTML = '';
        
        let activeCount = 0;
        let todayRevenue = 0;
        let revenue7Days = {}; 
        let peakData = new Array(24).fill(0);
        let durationData = [0, 0, 0, 0]; 

        snap.forEach((doc) => {
            const d = doc.data();
            const currentStatus = d.status ? d.status.toLowerCase().trim() : "";

            // --- ตารางรถที่กำลังจอด (Active) ---
            if (currentStatus === "parked" || currentStatus === "active") {
                activeCount++;
                if (activeBody) {
                    activeBody.innerHTML += `
                        <tr>
                            <td><b class="text-primary">${d.plate}</b></td>
                            <td>${d.entry_date} <span class="text-muted">| ${d.entry_time}</span></td>
                            <td><img src="${d.photo_url_in}" class="img-thumb" onclick="window.open(this.src)"></td>
                            <td class="text-center"><span class="badge bg-warning text-dark">กำลังจอด</span></td>
                        </tr>`;
                }
            } 
            
            // --- ตารางประวัติการเข้า-ออก (History) ---
            else if (currentStatus === "completed") {
                if (historyBody) {
                    historyBody.innerHTML += `
                        <tr>
                            <td><b class="text-dark">${d.plate}</b></td>
                            <td><small class="text-muted">${d.entry_date}</small><br><b>${d.entry_time}</b></td>
                            <td><small class="text-muted">${d.exit_date || d.entry_date}</small><br><b class="text-danger">${d.exit_time || '-'}</b></td>
                            <td class="text-center"><img src="${d.photo_url_in}" class="img-thumb" onclick="window.open(this.src)"></td>
                            <td class="text-center"><img src="${d.photo_url_out || d.photo_url_in}" class="img-thumb" onclick="window.open(this.src)"></td>
                            <td class="text-center"><span class="badge bg-light text-primary border">${d.duration_summary || '-'}</span></td>
                            <td class="text-end fw-bold">${(d.fee || 0).toLocaleString()} ฿</td>
                        </tr>`;
                }

                // สรุปรายได้วันนี้
                if (d.exit_date === todayStr) {
                    todayRevenue += (d.fee || 0);
                }

                // ข้อมูลกราฟรายได้ 7 วัน
                const dateKey = d.exit_date || d.entry_date;
                revenue7Days[dateKey] = (revenue7Days[dateKey] || 0) + (d.fee || 0);

                // ข้อมูลกราฟ Peak Hours (ดึงจากเวลาเข้า)
                if (d.entry_time) {
                    const hr = parseInt(d.entry_time.split(':')[0]);
                    if (!isNaN(hr)) peakData[hr]++;
                }
            }
        });

        // อัปเดตตัวเลขหน้า Dashboard
        const revenueElem = document.getElementById('today-revenue');
        if (revenueElem) revenueElem.innerText = todayRevenue.toLocaleString() + " ฿";

        const occupiedElem = document.getElementById('admin-occupied');
        if (occupiedElem) occupiedElem.innerText = activeCount;

        // เรียกฟังก์ชันวาดกราฟ (ถ้ามี)
        if (typeof updateCharts === "function") {
            updateCharts(revenue7Days, peakData, durationData);
        }

    }, (err) => console.error("Firestore Error:", err));
}

function updateCharts(revData, peakData, durData) {
    // 1. กราฟรายได้
    const labels = Object.keys(revData).sort().slice(-7);
    const values = labels.map(k => revData[k]);
    const ctx1 = document.getElementById('revenueChart');
    
    if (ctx1) {
        // ✅ เช็คก่อนว่ามี Chart เดิมอยู่จริงไหม และประกาศตัวแปรไว้ข้างนอกแล้ว
        if (revChart instanceof Chart) {
            revChart.destroy();
        }
        revChart = new Chart(ctx1, {
            type: 'bar',
            data: { 
                labels: labels, 
                datasets: [{ label: 'รายได้ (บาท)', data: values, backgroundColor: '#2ecc71' }] 
            },
            options: { responsive: true }
        });
    }

    // 2. กราฟ Peak Hours
    const ctx2 = document.getElementById('peakChart');
    if (ctx2) {
        if (pkChart instanceof Chart) {
            pkChart.destroy();
        }
        pkChart = new Chart(ctx2, {
            type: 'line',
            data: { 
                labels: Array.from({length: 24}, (_, i) => i + ":00"), 
                datasets: [{ label: 'จำนวนรถเข้า', data: peakData, borderColor: '#3498db', fill: true, backgroundColor: 'rgba(52, 152, 219, 0.1)' }] 
            }
        });
    }
    
    // ทำแบบเดียวกันกับ durChart และ occupancyChart...
}

// --- SEARCH LOGIC (FIXED TIMEZONE VERSION) ---
function searchVehicle() {
    const plateInput = document.getElementById('search-plate').value.trim();
    const resultDiv = document.getElementById('search-result');

    if (!plateInput) {
        alert("กรุณากรอกเลขทะเบียนรถ");
        return;
    }

    resultDiv.innerHTML = '<div class="text-center p-4"><div class="spinner-border text-primary"></div><p>กำลังคำนวณค่าบริการ...</p></div>';

    fs.collection("ParkingLogs").where("plate", "==", plateInput).orderBy("created_at", "desc").limit(1).get().then((snap) => {
        if (snap.empty) {
            resultDiv.innerHTML = `<div class="alert alert-danger">ไม่พบทะเบียน <b>${plateInput}</b></div>`;
            return;
        }

        snap.forEach((doc) => {
            const d = doc.data();
            const isParked = (d.status && d.status.toLowerCase().trim() === 'parked');
            let currentFee = d.fee || 0;
            let displayDuration = d.duration_summary || '-';

            if (isParked && d.created_at) {
                const nowMs = Date.now(); 
                const entryMs = d.created_at.toMillis(); 
                
                // แก้บั๊ก 7 ชม. โดยการบวกส่วนต่างที่หายไปคืน
                const diffMs = nowMs - entryMs;
                const correctedDiffMs = diffMs + (7 * 60 * 60 * 1000); 

                const totalMinutes = Math.max(1, Math.ceil(correctedDiffMs / (1000 * 60)));
                currentFee = totalMinutes * 10;
                
                const h = Math.floor(totalMinutes / 60);
                const m = totalMinutes % 60;
                displayDuration = `${h > 0 ? h + ' ชม. ' : ''}${m} นาที`;
            }

            resultDiv.innerHTML = `
                <div class="card border-0 shadow-lg" style="border-radius: 20px; overflow: hidden; border-left: 8px solid ${isParked ? '#ffa502' : '#2ed573'} !important;">
                    <div class="card-header ${isParked ? 'bg-warning' : 'bg-success'} text-dark fw-bold py-3 d-flex justify-content-between">
                        <span>ทะเบียน: ${d.plate}</span>
                        <span>${new Date().toLocaleTimeString('th-TH')}</span>
                    </div>
                    <div class="card-body p-4">
                        <div class="row align-items-center">
                            <div class="col-md-5 text-center mb-3 mb-md-0">
                                <img src="${d.photo_url_in}" class="img-fluid rounded shadow" style="height:180px; width:100%; object-fit:cover;">
                            </div>
                            <div class="col-md-7">
                                <h5 class="fw-bold">สถานะ: ${isParked ? 'กำลังจอด' : 'ออกแล้ว'}</h5>
                                <p class="mb-1">เข้า: ${d.entry_date} | ${d.entry_time}</p>
                                <p class="mb-1">เวลาจอดสะสม: <span class="text-primary fw-bold">${displayDuration}</span></p>
                                <hr>
                                <h3 class="text-danger fw-bold">${currentFee.toLocaleString()} ฿</h3>
                            </div>
                        </div>
                    </div>
                </div>`;
        });
    });
}