import firebase_admin
from firebase_admin import credentials, firestore, storage
from firebase_functions import https_fn
import requests
import base64
from datetime import datetime, timedelta
import math
import io
import time
import json
import cv2
import numpy as np

# --- 1. ตั้งค่า Firebase ---
if not firebase_admin._apps:
    firebase_admin.initialize_app()

def get_bucket():
    """ดึงค่า Bucket ของ Storage สำหรับเก็บรูปภาพ"""
    return storage.bucket("smart-parking-system-d6e6c.firebasestorage.app")

# --- 2. ข้อมูล API และการตั้งค่า ---
API_KEY = "c3DYfc6TLFfIVLOnX4yH3hIxU5qgbiEw"
LPR_URL = "https://api.aiforthai.in.th/lpr-iapp"
PARKING_RATE_PER_MIN = 10.0

# --- [ HELPER FUNCTIONS ] ---

def get_now_thailand():
    """ดึงเวลาปัจจุบันประเทศไทย (UTC+7)"""
    return datetime.utcnow() + timedelta(hours=7)

def process_image_cv2(img_binary):
    """ฟังก์ชันจัดการภาพ: แก้ปัญหา Mirror และปรับคุณภาพรูป"""
    try:
        nparr = np.frombuffer(img_binary, np.uint8)
        img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

        if img is None:
            print("⚠️ OpenCV: Cannot decode image")
            return img_binary
        
        # 🎯 กลับซ้ายขวา (แก้ Mirror)
        img = cv2.flip(img, 1)

        # บีบอัดภาพ
        _, buffer = cv2.imencode('.jpg', img, [int(cv2.IMWRITE_JPEG_QUALITY), 90])
        return buffer.tobytes()
    except Exception as e:
        print(f"❌ Image Processing Error: {e}")
        return img_binary

def upload_to_storage(img_binary, folder, plate):
    """อัปโหลดรูปภาพขึ้น Firebase Storage"""
    try:
        now_th = get_now_thailand()
        timestamp = now_th.strftime("%d-%m-%Y_%H:%M:%S")
        filename = f"{folder}/{timestamp}_{plate}.jpg"
        
        bucket = get_bucket()
        blob = bucket.blob(filename)
        blob.upload_from_string(img_binary, content_type='image/jpeg')
        blob.make_public()
        return blob.public_url
    except Exception as e:
        print(f"❌ Storage Upload Error: {e}")
        return None

def get_plate_from_api_binary(img_binary):
    """ส่งรูปภาพไปยัง AI พร้อมระบบจัดการ Error"""
    processed_img = process_image_cv2(img_binary)
    
    max_retries = 2
    for attempt in range(1, max_retries + 1):
        try:
            print(f"📡 [AI Processing] Attempt {attempt}/{max_retries}")
            headers = { 'apikey': API_KEY }
            files = [('file', ('image.jpg', io.BytesIO(processed_img), 'image/jpeg'))]
            response = requests.post(LPR_URL, headers=headers, files=files, timeout=15)
            
            if response.status_code == 200:
                res_data = response.json()
                lp = res_data.get('lp_number', '')
                raw_province = res_data.get('province', '')
                
                if lp and lp not in ["null", ""]:
                    province = raw_province.split("(")[1].split(")")[0] if "(" in raw_province else raw_province
                    result = f"{lp}{province}".replace(" ", "").strip()
                    print(f"✅ AI Found Plate: {result}")
                    return result, processed_img
            
            print(f"⚠️ AI Status: {response.status_code}")
        except Exception as e:
            print(f"❌ AI API Error: {e}")
        
        if attempt < max_retries:
            time.sleep(1.5)
            
    return "UNKNOWN", processed_img

# --- [ FUNCTION: ขาเข้า (handle_entry) ] ---

@https_fn.on_request(region="asia-southeast1")
def handle_entry(req: https_fn.Request) -> https_fn.Response:
    if req.method != 'POST':
        return https_fn.Response("Method Not Allowed", status=405)

    db = firestore.client()
    try:
        img_binary = req.data if req.content_type == 'image/jpeg' else base64.b64decode(req.get_json().get('image', ''))
        
        if not img_binary:
            return https_fn.Response(json.dumps({"status": "failed", "msg": "NO IMAGE"}), mimetype="application/json")

        plate, processed_img = get_plate_from_api_binary(img_binary)
        photo_url = upload_to_storage(processed_img, "entry_photos", plate)

        now_th = get_now_thailand()
        date_str = now_th.strftime("%d-%m-%Y")
        time_str = now_th.strftime("%H:%M:%S")
        
        doc_id = f"{now_th.strftime('%d-%m-%Y_%H:%M:%S')}_{plate}"
        doc_ref = db.collection('ParkingLogs').document(doc_id)
        
        doc_ref.set({
            'plate': plate,
            'entry_date': date_str,
            'entry_time': time_str,
            'photo_url_in': photo_url,
            'exit_date': None,
            'exit_time': None,
            'fee': 0,
            'duration_summary': "",
            'status': 'parked',
            'created_at': now_th 
        })
        
        resp_data = {
            "status": "success",
            "msg": "ENTRY OK",
            "plate": "WELCOME",
            "time": time_str[:5]
        }
        
        return https_fn.Response(json.dumps(resp_data), mimetype="application/json")
    except Exception as e:
        err_msg = str(e)[:15]
        return https_fn.Response(json.dumps({"status": "failed", "msg": f"ERR: {err_msg}"}), mimetype="application/json")

# --- [ FUNCTION: ขาออก (handle_exit) ] ---

@https_fn.on_request(region="asia-southeast1")
def handle_exit(req: https_fn.Request) -> https_fn.Response:
    if req.method != 'POST':
        return https_fn.Response("Method Not Allowed", status=405)

    db = firestore.client()
    try:
        # 1. รับภาพและอ่านทะเบียน
        img_binary = req.data if req.content_type == 'image/jpeg' else base64.b64decode(req.get_json().get('image', ''))
        plate, processed_img = get_plate_from_api_binary(img_binary)

        if plate == "UNKNOWN":
            return https_fn.Response(json.dumps({"status": "failed", "msg": "READ FAILED"}), mimetype="application/json")

        # 2. ค้นหาข้อมูล (เลี่ยง Index)
        docs_stream = db.collection('ParkingLogs') \
                        .where('plate', '==', plate) \
                        .where('status', '==', 'parked') \
                        .stream()

        found_docs = []
        for doc in docs_stream:
            data = doc.to_dict()
            if 'created_at' in data:
                found_docs.append({"id": doc.id, "data": data})

        if not found_docs:
            print(f"❌ Plate {plate} not found")
            return https_fn.Response(json.dumps({"status": "failed", "msg": "NOT REGISTERED"}), mimetype="application/json")

        # 3. เรียงคันล่าสุดและอัปโหลดรูปขาออก
        found_docs.sort(key=lambda x: x['data']['created_at'], reverse=True)
        log_id = found_docs[0]['id']
        entry_data = found_docs[0]['data']

        photo_url_out = upload_to_storage(processed_img, "exit_photos", plate)
        now_th = get_now_thailand()

        # 4. คำนวณเวลาและค่าจอด
        t_entry = entry_data['created_at']
        if isinstance(t_entry, str):
            t_entry = datetime.strptime(t_entry, "%d-%m-%Y %H:%M:%S")
            
        diff = now_th.replace(tzinfo=None) - t_entry.replace(tzinfo=None)
        total_sec = max(0, int(diff.total_seconds()))
        
        days = total_sec // 86400
        hours = (total_sec % 86400) // 3600
        minutes = (total_sec % 3600) // 60
        dur_summary = f"{days}D {hours}H {minutes}M" if days > 0 else f"{hours}H {minutes}M"
        
        total_payable_min = math.ceil(max(1, total_sec) / 60)
        fee = total_payable_min * PARKING_RATE_PER_MIN

        # 5. อัปเดตข้อมูลขาออก
        db.collection('ParkingLogs').document(log_id).update({
            'exit_date': now_th.strftime("%d-%m-%Y"),
            'exit_time': now_th.strftime("%H:%M:%S"),
            'photo_url_out': photo_url_out,
            'fee': fee,
            'duration_summary': dur_summary,
            'total_seconds': total_sec,
            'status': 'completed'
        })
        
        # 🎯 เตรียมส่งข้อมูลกลับ (ใช้ "duration" ตามที่ ESP32 รอรับ)
        resp_data = {
            "status": "success", 
            "msg": "EXIT OK",
            "fee": int(fee), 
            "duration": dur_summary
        }
        
        print(f"✅ Exit Done: {plate}, Fee: {fee}")
        return https_fn.Response(json.dumps(resp_data), mimetype="application/json")

    except Exception as e:
        print(f"🔥 Exit Exception: {e}")
        # ส่งข้อความ Error สั้นๆ กลับไปให้บอร์ดโชว์
        err_msg = str(e)[:15]
        return https_fn.Response(json.dumps({"status": "failed", "msg": f"ERR: {err_msg}"}), mimetype="application/json")