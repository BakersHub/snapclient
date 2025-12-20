// LED Control Functions
function updateBrightnessDisplay() {
	document.getElementById('brightnessValue').textContent = document.getElementById('ledBrightness').value;
}

function updateSpeedDisplay() {
	document.getElementById('speedValue').textContent = document.getElementById('ledSpeed').value;
}

function updateSensitivityDisplay() {
	document.getElementById('sensitivityValue').textContent = document.getElementById('ledSensitivity').value;
}

function hexToRgb(hex) {
	var result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
	return result ? {
		r: parseInt(result[1], 16),
		g: parseInt(result[2], 16),
		b: parseInt(result[3], 16)
	} : null;
}

function updateLED() {
	var effect = document.getElementById('ledEffect').value;
	var brightness = document.getElementById('ledBrightness').value;
	var speed = document.getElementById('ledSpeed').value;
	var sensitivity = document.getElementById('ledSensitivity').value;
	var colorHex = document.getElementById('ledColor').value;
	var rgb = hexToRgb(colorHex);
	
	var params = 'effect=' + effect + 
				'&brightness=' + brightness + 
				'&speed=' + speed + 
				'&sensitivity=' + sensitivity +
				'&color=' + rgb.r + ',' + rgb.g + ',' + rgb.b;
	
	document.getElementById('ledStatus').textContent = 'Updating...';
	
	fetch('/led', {
		method: 'POST',
		headers: {
			'Content-Type': 'application/x-www-form-urlencoded',
		},
		body: params
	})
	.then(response => response.text())
	.then(data => {
		document.getElementById('ledStatus').textContent = '✓ ' + data;
		setTimeout(() => { document.getElementById('ledStatus').textContent = 'Ready'; }, 2000);
	})
	.catch(error => {
		document.getElementById('ledStatus').textContent = '✗ Error: ' + error;
		document.getElementById('ledStatus').style.background = '#ffebee';
		document.getElementById('ledStatus').style.color = '#c62828';
	});
}

function loadLEDConfig() {
	fetch('/led')
		.then(response => response.json())
		.then(data => {
			document.getElementById('ledEffect').value = data.effect;
			document.getElementById('ledBrightness').value = data.brightness;
			document.getElementById('ledSpeed').value = data.speed;
			document.getElementById('ledSensitivity').value = data.sensitivity;
			
			var hex = '#' + 
					('0' + data.color.r.toString(16)).slice(-2) +
					('0' + data.color.g.toString(16)).slice(-2) +
					('0' + data.color.b.toString(16)).slice(-2);
			document.getElementById('ledColor').value = hex;
			
			updateBrightnessDisplay();
			updateSpeedDisplay();
			updateSensitivityDisplay();
			
			document.getElementById('ledStatus').textContent = 'Connected (' + data.num_leds + ' LEDs)';
		})
		.catch(error => {
			document.getElementById('ledController').style.display = 'none';
			console.log('LED controller not available');
		});
}

// Load LED config on page load
window.addEventListener('load', function() {
	loadLEDConfig();
});
