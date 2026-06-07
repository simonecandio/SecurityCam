package com.securitycam.repository

import android.graphics.Bitmap
import android.graphics.RectF
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.label.ImageLabeling
import com.google.mlkit.vision.label.defaults.ImageLabelerOptions
import com.google.mlkit.vision.objects.ObjectDetection
import com.google.mlkit.vision.objects.defaults.ObjectDetectorOptions
import com.securitycam.model.DetectionCategories
import com.securitycam.model.DetectionResult
import kotlinx.coroutines.tasks.await

class MlKitRepository {

    // Object Detection per persone
    private val objectOptions = ObjectDetectorOptions.Builder()
        .setDetectorMode(ObjectDetectorOptions.SINGLE_IMAGE_MODE)
        .enableClassification()
        .enableMultipleObjects()
        .build()
    private val objectDetector = ObjectDetection.getClient(objectOptions)

    // Image Labeling per animali, veicoli, pacchi
    private val labelOptions = ImageLabelerOptions.Builder()
        .setConfidenceThreshold(0.40f)
        .build()
    private val labeler = ImageLabeling.getClient(labelOptions)

    suspend fun detectAll(bitmap: Bitmap, frameIndex: Int): DetectionResult {
        return try {
            val image = InputImage.fromBitmap(bitmap, 0)
            val detectedCategories = mutableSetOf<String>()
            var maxConf = 0f
            var bestLabel = ""
            var personFound = false

            // 1. Object Detection per persone
            val objects = objectDetector.process(image).await()
            for (obj in objects) {
                for (label in obj.labels) {
                    val isPerson = label.text.equals("Person", ignoreCase = true)
                            || label.text.equals("Fashion good", ignoreCase = true)
                            || label.index == 0
                    if (isPerson && label.confidence > 0.3f) {
                        personFound = true
                        detectedCategories.add(DetectionCategories.PERSON)
                        if (label.confidence > maxConf) {
                            maxConf = label.confidence
                            bestLabel = "Person"
                        }
                    }
                }
            }

            // 2. Image Labeling: copre persone (primi piani) + animali/veicoli/pacchi
            val labels = labeler.process(image).await()
            for (label in labels) {
                // controlla anche label generiche per persone
                val isPersonLabel = label.text.equals("Person", ignoreCase = true)
                        || label.text.equals("Face", ignoreCase = true)
                        || label.text.equals("Selfie", ignoreCase = true)
                        || label.text.equals("Smile", ignoreCase = true)
                        || label.text.equals("Forehead", ignoreCase = true)
                        || label.text.equals("Chin", ignoreCase = true)
                        || label.text.equals("Eyebrow", ignoreCase = true)
                        || label.text.equals("Glasses", ignoreCase = true)
                if (isPersonLabel && label.confidence > 0.6f) {
                    personFound = true
                    detectedCategories.add(DetectionCategories.PERSON)
                    if (label.confidence > maxConf) {
                        maxConf = label.confidence
                        bestLabel = label.text
                    }
                    continue
                }

                val category = DetectionCategories.labelToCategory(label.text)
                if (category != null && category != DetectionCategories.PERSON) {
                    detectedCategories.add(category)
                    if (label.confidence > maxConf) {
                        maxConf = label.confidence
                        bestLabel = label.text
                    }
                }
            }

            DetectionResult(
                frameIndex = frameIndex,
                personDetected = personFound,
                confidence = maxConf,
                detectedCategories = detectedCategories.toList(),
                detectedLabel = bestLabel
            )
        } catch (e: Exception) {
            DetectionResult(frameIndex = frameIndex)
        }
    }

    suspend fun detectPersons(bitmap: Bitmap, frameIndex: Int): DetectionResult {
        return detectAll(bitmap, frameIndex)
    }

    /**
     * Risultato di una detection con bounding box.
     * Usato nella StreamScreen per disegnare i rettangoli.
     */
    data class BoxDetection(
        val box: RectF,
        val label: String,       // "Person", "Cat", "Dog"
        val confidence: Float
    )

    /**
     * Detection con bounding box per lo streaming live.
     *
     * Combina Object Detection (per i box) + Image Labeling (per le label):
     * 1. Object Detection trova dove sono gli oggetti nell'immagine
     * 2. Per ogni oggetto, croppa il bitmap e lo passa a Image Labeling
     * 3. Image Labeling identifica la categoria (Person, Cat, Dog)
     *
     * Solo le detection con confidence > threshold vengono ritornate.
     */
    suspend fun detectWithBoxes(bitmap: Bitmap, threshold: Float = 0.5f): List<BoxDetection> {
        return try {
            val image = InputImage.fromBitmap(bitmap, 0)
            val objects = objectDetector.process(image).await()

            val results = mutableListOf<BoxDetection>()

            for (obj in objects) {
                val box = obj.boundingBox

                // Croppa il bitmap alla regione dell'oggetto
                val cropLeft = box.left.coerceAtLeast(0)
                val cropTop = box.top.coerceAtLeast(0)
                val cropRight = box.right.coerceAtMost(bitmap.width)
                val cropBottom = box.bottom.coerceAtMost(bitmap.height)
                val cropW = cropRight - cropLeft
                val cropH = cropBottom - cropTop
                if (cropW <= 10 || cropH <= 10) continue

                try {
                    val crop = Bitmap.createBitmap(bitmap, cropLeft, cropTop, cropW, cropH)
                    val cropImage = InputImage.fromBitmap(crop, 0)
                    val labels = labeler.process(cropImage).await()

                    // Mappa le label generiche di Image Labeling alle nostre 3 categorie
                    val targetLabels = mapOf(
                        "Person" to "Person", "Man" to "Person", "Woman" to "Person",
                        "Human face" to "Person", "Selfie" to "Person", "Smile" to "Person",
                        "Face" to "Person", "Forehead" to "Person", "Glasses" to "Person",
                        "Cat" to "Cat",
                        "Dog" to "Dog"
                    )

                    var bestLabel = ""
                    var bestConf = 0f

                    for (label in labels) {
                        val mapped = targetLabels[label.text]
                        if (mapped != null && label.confidence > bestConf) {
                            bestLabel = mapped
                            bestConf = label.confidence
                        }
                    }

                    if (bestConf >= threshold && bestLabel.isNotEmpty()) {
                        results.add(BoxDetection(
                            box = RectF(
                                box.left.toFloat(), box.top.toFloat(),
                                box.right.toFloat(), box.bottom.toFloat()
                            ),
                            label = bestLabel,
                            confidence = bestConf
                        ))
                    }
                } catch (_: Exception) { /* crop fallito, skip */ }
            }

            results
        } catch (_: Exception) {
            emptyList()
        }
    }
}