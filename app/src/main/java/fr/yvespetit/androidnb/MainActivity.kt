package fr.yvespetit.androidnb

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(modifier = Modifier.fillMaxSize()) { MainScreen() }
            }
        }
    }
}

/**
 * First screen: board address and dial frequency. The connection to the nbd
 * daemon (audio, waterfall, beacon lock status) arrives with step 4 of the plan.
 */
@Composable
private fun MainScreen() {
    var host by rememberSaveable { mutableStateOf("192.168.2.1") }
    var dial by rememberSaveable { mutableStateOf("10489,900") }
    val downlink = Qo100.parseMHz(dial)

    Column(
        modifier = Modifier.padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("QO-100 NB", style = MaterialTheme.typography.headlineSmall)
        OutlinedTextField(value = host, onValueChange = { host = it }, label = { Text("Adresse de la carte") })
        OutlinedTextField(value = dial, onValueChange = { dial = it }, label = { Text("Fréquence de réception (MHz)") })
        if (downlink == null) {
            Text("Fréquence invalide", color = MaterialTheme.colorScheme.error)
        } else {
            val uplink = Qo100.uplinkFor(downlink)
            Text("Réception : ${Qo100.formatMHz(downlink)}")
            Text("Émission : ${Qo100.formatMHz(uplink)}" + if (Qo100.isTransmitAllowed(uplink)) "" else "  (hors transpondeur NB)")
        }
        Text(
            "Connexion au démon nbd : à venir (audio Opus, waterfall, état du verrou balise).",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
