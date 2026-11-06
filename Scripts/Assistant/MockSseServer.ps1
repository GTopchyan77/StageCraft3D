# A local stand-in for the Messages API's streaming answer (Docs/ADR/0030 section 8, Phase H0, STATE.md #72). It answers each POST with
# a text/event-stream shaped like the real one (message_start with usage, a text block in several deltas, message_delta, message_stop),
# one event every DelayMs, so StageCraft.Assistant.Spike Url=http://127.0.0.1:<Port>/v1/messages can show whether the engine's HTTP
# module hands the body over as it arrives. No key, no network beyond this PC, no cost. The first request reports a cache write and
# the next ones a cache read, like a cached prefix.
#
#   powershell -ExecutionPolicy Bypass -File Scripts\Assistant\MockSseServer.ps1 [-Port 8787] [-Requests 2] [-DelayMs 150]
param(
	[int]$Port = 8787,
	[int]$Requests = 2,
	[int]$DelayMs = 150
)

$ErrorActionPreference = 'Stop'
$Listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$Listener.Start()
Write-Output "Mock SSE server on http://127.0.0.1:$Port/v1/messages for $Requests request(s), $DelayMs ms between events."
try {
	for ($Served = 0; $Served -lt $Requests; $Served++) {
		$Client = $Listener.AcceptTcpClient()
		$Stream = $Client.GetStream()
		$Reader = [System.IO.StreamReader]::new($Stream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
		$Length = 0
		while ($true) {
			$Line = $Reader.ReadLine()
			if ($null -eq $Line -or $Line -eq '') { break }
			if ($Line -match '^Content-Length:\s*(\d+)') { $Length = [int]$Matches[1] }
		}
		$Buffer = New-Object char[] $Length
		$Read = 0
		while ($Read -lt $Length) {
			$Count = $Reader.Read($Buffer, $Read, $Length - $Read)
			if ($Count -le 0) { break }
			$Read += $Count
		}
		Write-Output "Request $($Served + 1): $Read body bytes."

		$Write = {
			param([string]$Text)
			$Bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
			$Stream.Write($Bytes, 0, $Bytes.Length)
			$Stream.Flush()
		}
		& $Write "HTTP/1.1 200 OK`r`nContent-Type: text/event-stream`r`nCache-Control: no-cache`r`nConnection: close`r`n`r`n"
		$CacheWrite = if ($Served -eq 0) { 1800 } else { 0 }
		$CacheRead = if ($Served -eq 0) { 0 } else { 1800 }
		$Events = @(
			"event: message_start`ndata: {`"type`":`"message_start`",`"message`":{`"id`":`"msg_mock`",`"type`":`"message`",`"role`":`"assistant`",`"model`":`"mock`",`"content`":[],`"usage`":{`"input_tokens`":40,`"cache_creation_input_tokens`":$CacheWrite,`"cache_read_input_tokens`":$CacheRead,`"output_tokens`":1}}}`n`n",
			": keep-alive comment`n`n",
			"event: content_block_start`ndata: {`"type`":`"content_block_start`",`"index`":0,`"content_block`":{`"type`":`"text`",`"text`":`"`"}}`n`n",
			"event: content_block_delta`ndata: {`"type`":`"content_block_delta`",`"index`":0,`"delta`":{`"type`":`"text_delta`",`"text`":`"BLACKOUT is `"}}`n`n",
			"event: content_block_delta`ndata: {`"type`":`"content_block_delta`",`"index`":0,`"delta`":{`"type`":`"text_delta`",`"text`":`"in the Playback panel, `"}}`n`n",
			"event: content_block_delta`ndata: {`"type`":`"content_block_delta`",`"index`":0,`"delta`":{`"type`":`"text_delta`",`"text`":`"under the Grand Master.`"}}`n`n",
			"event: content_block_stop`ndata: {`"type`":`"content_block_stop`",`"index`":0}`n`n",
			"event: message_delta`ndata: {`"type`":`"message_delta`",`"delta`":{`"stop_reason`":`"end_turn`"},`"usage`":{`"output_tokens`":14}}`n`n",
			"event: message_stop`ndata: {`"type`":`"message_stop`"}`n`n"
		)
		foreach ($Event in $Events) {
			Start-Sleep -Milliseconds $DelayMs
			& $Write $Event
		}
		$Client.Close()
	}
}
finally {
	$Listener.Stop()
}
