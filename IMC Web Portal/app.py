import streamlit as st
import requests
import pandas as pd
import plotly.express as px # Fixed Import
from datetime import datetime
import time

# ==========================================
# CORE CONFIGURATION
# ==========================================
WEB_API_KEY = "AIzaSyBuMFYY3y5kA0Tp451Ki4eqZ28NhdZA6pw"
DATABASE_URL = "https://jal-rakshak-indore-default-rtdb.asia-southeast1.firebasedatabase.app"
USER_EMAIL = "varunsontakke2@gmail.com"
USER_PASS = "Varuna@#123"

# ==========================================
# FIREBASE API UTILS
# ==========================================
@st.cache_data(ttl=300)
def get_token():
    url = f"https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key={WEB_API_KEY}"
    resp = requests.post(url, json={"email": USER_EMAIL, "password": USER_PASS, "returnSecureToken": True})
    return resp.json().get("idToken")

def fetch_db(token, path):
    endpoint = f"{DATABASE_URL}/{path}.json?auth={token}"
    try: return requests.get(endpoint).json()
    except: return None

# ==========================================
# UI DASHBOARD
# ==========================================
st.set_page_config(page_title="Jal Rakshak IMC", layout="wide")
st.title("💧 Jal Rakshak: Infrastructure Monitoring Center")

tab1, tab2 = st.tabs(["📡 Live Mesh Nodes", "🧪 Automated Water Lab"])

# --- TAB 1: NODE MONITORING ---
with tab1:
    node_id = st.selectbox("Select Active Pipe Node:", [1, 2, 3, 4], key="node_sel")
    token = get_token()
    data = fetch_db(token, f"nodes/node_{node_id}")
    
    if data:
        # Metrics Row
        c1, c2, c3, c4, c5 = st.columns(5)
        c1.metric("pH", f"{data.get('ph', 0):.2f}")
        c2.metric("TDS", f"{data.get('tds', 0):.0f} ppm")
        c3.metric("Turbidity", f"{data.get('turbidity', 0):.2f} NTU")
        c4.metric("Temp", f"{data.get('temperature', 0):.1f} °C")
        c5.metric("Leak Status", "ALERT" if data.get('leakAlert') else "OK")

        # Visualization
        st.subheader("Sensor Time-Series Analysis")
        df = pd.DataFrame([data])
        df['Time'] = datetime.now().strftime("%H:%M:%S")
        
        # Now 'px' is correctly defined
        fig = px.bar(df, x='Time', y=['ph', 'tds', 'turbidity', 'temperature'], 
                     title=f"Live Telemetry - Node {node_id}", barmode='group')
        st.plotly_chart(fig, use_container_width=True)
        
        st.subheader("📍 Live Node Map")
        st.map(pd.DataFrame({'lat': [data.get('latitude', 22.7196)], 'lon': [data.get('longitude', 75.8577)]}))
    else:
        st.info(f"Node {node_id} currently offline or awaiting transmission...")

# --- TAB 2: AUTOMATED LAB LOG ---
with tab2:
    st.subheader("🧪 Automated Sewage Analysis Log")
    lab_data = fetch_db(token, "lab_results")
    if lab_data:
        lab_df = pd.DataFrame.from_dict(lab_data, orient='index')
        st.dataframe(lab_df.sort_index(ascending=False), use_container_width=True)
    else:
        st.warning("No automated water tests logged yet.")

if st.sidebar.checkbox("Enable Live Refresh", True):
    time.sleep(3)
    st.rerun()