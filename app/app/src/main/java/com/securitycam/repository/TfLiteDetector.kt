package com.securitycam.repository

import android.content.Context
import android.graphics.Bitmap
import android.graphics.RectF
import org.tensorflow.lite.Interpreter
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.MappedByteBuffer
import java.nio.channels.FileChannel

/**
 * Wrapper per il modello YOLOv8 TFLite che esegue object detection
 * con bounding box sullo streaming live della camera ESP32.
 *
 */
class TfLiteDetector(context: Context) {

    data class Detection(
        val boundingBox: RectF,   // coordinate in pixel dell'immagine originale
        val label: String,        // "person", "cat", o "dog"
        val confidence: Float     // 0.0 - 1.0
    )

    companion object {
        private const val MODEL_FILE = "security_cam_detector.tflite"
        private const val INPUT_SIZE = 320          // deve matchare imgsz del training
        private const val CONFIDENCE_THRESHOLD = 0.4f
        private val CLASS_NAMES = arrayOf("person", "cat", "dog")
    }

    private val interpreter: Interpreter

    init {
        val options = Interpreter.Options()
        options.setNumThreads(4)
        // GPU delegate rimosso: crasha sull'emulatore Android
        // (libEGL: call to OpenGL ES API with no current context).
        // La CPU con 4 thread e' sufficiente per YOLOv8n (~40-80ms).
        interpreter = Interpreter(loadModelFile(context), options)
    }

    /**
     * Esegue object detection su un Bitmap.
     * Ritorna una lista di Detection con box, label e confidence.
     */
    fun detect(bitmap: Bitmap): List<Detection> {
        val resized = Bitmap.createScaledBitmap(bitmap, INPUT_SIZE, INPUT_SIZE, true)

        // Prepara l'input: bitmap -> ByteBuffer float32 normalizzato
        val inputBuffer = ByteBuffer.allocateDirect(1 * INPUT_SIZE * INPUT_SIZE * 3 * 4)
        inputBuffer.order(ByteOrder.nativeOrder())
        inputBuffer.rewind()

        val pixels = IntArray(INPUT_SIZE * INPUT_SIZE)
        resized.getPixels(pixels, 0, INPUT_SIZE, 0, 0, INPUT_SIZE, INPUT_SIZE)

        for (pixel in pixels) {
            // Normalizza RGB da 0-255 a 0.0-1.0
            inputBuffer.putFloat(((pixel shr 16) and 0xFF) / 255.0f) // R
            inputBuffer.putFloat(((pixel shr 8) and 0xFF) / 255.0f)  // G
            inputBuffer.putFloat((pixel and 0xFF) / 255.0f)           // B
        }

        // Output di YOLOv8: [1, num_classes + 4, num_detections]
        // dove ogni detection ha [x_center, y_center, width, height, class_scores...]
        // Il formato esatto dipende dalla versione e dall'export.
        // Per l'export TFLite di ultralytics, l'output e' tipicamente
        // [1, 7, 2100] o simile — bisogna trasporre.
        //
        // Proviamo a determinare il formato dall'output shape.
        val outputTensor = interpreter.getOutputTensor(0)
        val outputShape = outputTensor.shape() // es: [1, 7, 2100]

        val outputBuffer = Array(1) {
            Array(outputShape[1]) {
                FloatArray(outputShape[2])
            }
        }

        // Inference
        interpreter.run(inputBuffer, outputBuffer)

        // Parse delle detections
        return parseDetections(outputBuffer[0], outputShape, bitmap.width, bitmap.height)
    }

    /**
     * Parsa l'output di YOLOv8 in Detection objects.
     *
     * L'output di YOLOv8 TFLite e' [num_features, num_detections] dove:
     * - feature 0-3: x_center, y_center, width, height (normalizzati 0-1)
     * - feature 4+: confidence per ogni classe
     *
     * Se il formato e' trasposto ([num_detections, num_features]), si adatta.
     */
    private fun parseDetections(
        output: Array<FloatArray>,
        shape: IntArray,
        imgWidth: Int,
        imgHeight: Int
    ): List<Detection> {
        val detections = mutableListOf<Detection>()
        val numClasses = CLASS_NAMES.size

        // Determina se l'output e' [features, detections] o [detections, features]
        val dim1 = shape[1]
        val dim2 = shape[2]
        val isTransposed = dim1 == (4 + numClasses)  // [7, 2100]

        val numDetections = if (isTransposed) dim2 else dim1
        val numFeatures = if (isTransposed) dim1 else dim2

        if (numFeatures < 4 + numClasses) return detections

        for (i in 0 until numDetections) {
            // Leggi i valori in base all'orientamento
            val xc: Float
            val yc: Float
            val w: Float
            val h: Float
            val classScores = FloatArray(numClasses)

            if (isTransposed) {
                // output[feature][detection]
                xc = output[0][i]
                yc = output[1][i]
                w = output[2][i]
                h = output[3][i]
                for (c in 0 until numClasses) {
                    classScores[c] = output[4 + c][i]
                }
            } else {
                // output[detection][feature]
                xc = output[i][0]
                yc = output[i][1]
                w = output[i][2]
                h = output[i][3]
                for (c in 0 until numClasses) {
                    classScores[c] = output[i][4 + c]
                }
            }

            // Trova la classe con confidence massima
            var maxConf = 0f
            var maxIdx = 0
            for (c in classScores.indices) {
                if (classScores[c] > maxConf) {
                    maxConf = classScores[c]
                    maxIdx = c
                }
            }

            if (maxConf < CONFIDENCE_THRESHOLD) continue

            // Converti da coordinate normalizzate a pixel
            val scaleX = imgWidth.toFloat() / INPUT_SIZE
            val scaleY = imgHeight.toFloat() / INPUT_SIZE

            val left = (xc - w / 2) * INPUT_SIZE * scaleX
            val top = (yc - h / 2) * INPUT_SIZE * scaleY
            val right = (xc + w / 2) * INPUT_SIZE * scaleX
            val bottom = (yc + h / 2) * INPUT_SIZE * scaleY

            detections.add(Detection(
                boundingBox = RectF(
                    left.coerceIn(0f, imgWidth.toFloat()),
                    top.coerceIn(0f, imgHeight.toFloat()),
                    right.coerceIn(0f, imgWidth.toFloat()),
                    bottom.coerceIn(0f, imgHeight.toFloat())
                ),
                label = CLASS_NAMES.getOrElse(maxIdx) { "?" },
                confidence = maxConf
            ))
        }

        // NMS (Non-Maximum Suppression) semplificata:
        // rimuovi box che si sovrappongono troppo
        return nonMaxSuppression(detections, iouThreshold = 0.5f)
    }

    /**
     * Non-Maximum Suppression: se due box si sovrappongono per piu'
     * del 50% (IoU > 0.5), tiene solo quello con confidence piu' alta.
     * Evita di avere 5 rettangoli sulla stessa persona.
     */
    private fun nonMaxSuppression(
        detections: List<Detection>,
        iouThreshold: Float
    ): List<Detection> {
        val sorted = detections.sortedByDescending { it.confidence }.toMutableList()
        val result = mutableListOf<Detection>()

        while (sorted.isNotEmpty()) {
            val best = sorted.removeAt(0)
            result.add(best)

            sorted.removeAll { other ->
                other.label == best.label &&
                        computeIoU(best.boundingBox, other.boundingBox) > iouThreshold
            }
        }

        return result
    }

    /**
     * Calcola Intersection over Union tra due rettangoli.
     * IoU = area_intersezione / area_unione
     * Usato nella NMS per capire quanto due box si sovrappongono.
     */
    private fun computeIoU(a: RectF, b: RectF): Float {
        val interLeft = maxOf(a.left, b.left)
        val interTop = maxOf(a.top, b.top)
        val interRight = minOf(a.right, b.right)
        val interBottom = minOf(a.bottom, b.bottom)

        val interArea = maxOf(0f, interRight - interLeft) * maxOf(0f, interBottom - interTop)
        val aArea = a.width() * a.height()
        val bArea = b.width() * b.height()
        val unionArea = aArea + bArea - interArea

        return if (unionArea > 0) interArea / unionArea else 0f
    }

    /**
     * Carica il modello .tflite dalla cartella assets.
     */
    private fun loadModelFile(context: Context): MappedByteBuffer {
        val fd = context.assets.openFd(MODEL_FILE)
        val inputStream = FileInputStream(fd.fileDescriptor)
        val channel = inputStream.channel
        return channel.map(FileChannel.MapMode.READ_ONLY, fd.startOffset, fd.declaredLength)
    }

    /**
     * Libera le risorse. Chiamare quando non serve piu' il detector.
     */
    fun close() {
        interpreter.close()
    }
}