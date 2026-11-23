package ng.kithso.android.cast

import android.graphics.Bitmap
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.opengl.GLUtils
import android.view.Surface
import ng.kithso.android.ui.theme.BackdropStrength
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max

/**
 * Draws the cast's video into an encoder's input [surface]: the app's art backdrop as
 * [MusicMotion] moves it, and a [CastPicture] over it, fading from one to the next. Use it, from
 * making it to [release], on one thread.
 */
class CastRenderer(surface: Surface, private val width: Int, private val height: Int) {
    private val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var window: EGLSurface = EGL14.EGL_NO_SURFACE
    private val program: Int
    private val textures = IntArray(2)
    private val quad = ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).asFloatBuffer()
        .put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)).apply { position(0) }

    // Which of the textures shows, the other being what it fades from; and how far it has faded.
    private var front = 0
    private var fade = 1f
    // The glows' colours, of what's faded from, what's shown now, and what's faded to.
    // All are the bare background's to begin with, so the first glows come up from it.
    private val fromColors = FloatArray(GLOWS * 3) { SURFACE[it % 3] }
    private val colors = fromColors.copyOf()
    private val toColors = fromColors.copyOf()
    private val glowData = FloatArray(GLOWS * 4)
    private var frame = 0

    init {
        try {
            val version = IntArray(2)
            check(EGL14.eglInitialize(display, version, 0, version, 1)) { "No EGL" }
            val configs = arrayOfNulls<EGLConfig>(1)
            val found = IntArray(1)
            val wanted = intArrayOf(
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL_RECORDABLE_ANDROID, 1,
                EGL14.EGL_NONE,
            )
            check(EGL14.eglChooseConfig(display, wanted, 0, configs, 0, 1, found, 0) && found[0] > 0) { "No EGL config" }
            context = EGL14.eglCreateContext(
                display, configs[0], EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
            )
            window = EGL14.eglCreateWindowSurface(display, configs[0], surface, intArrayOf(EGL14.EGL_NONE), 0)
            check(EGL14.eglMakeCurrent(display, window, window, context)) { "No EGL surface: ${EGL14.eglGetError()}" }
            program = link()
            GLES20.glGenTextures(2, textures, 0)
            val clear = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
            textures.forEach { upload(it, clear) }
            clear.recycle()
        } catch (e: Exception) {
            release()
            throw e
        }
    }

    /** Whether it's still fading to the last picture; another would cut that short. */
    val fading get() = fade < 1f

    /** Fades to [picture] over the frames to come. */
    fun show(picture: CastPicture) {
        front = 1 - front
        upload(textures[front], picture.card)
        colors.copyInto(fromColors)
        for (i in 0 until GLOWS) {
            val argb = picture.colors.getOrElse(i) { 0 }
            toColors[i * 3] = (argb shr 16 and 0xff) / 255f
            toColors[i * 3 + 1] = (argb shr 8 and 0xff) / 255f
            toColors[i * 3 + 2] = (argb and 0xff) / 255f
        }
        fade = 0f
    }

    /** Draws a frame that's [seconds] after the last, at [us] in the stream. It may wait for the encoder. */
    fun draw(glows: List<Glow>, seconds: Float, us: Long) {
        fade = (fade + seconds / FADE_S).coerceAtMost(1f)
        val eased = fade * fade * (3 - 2 * fade)
        for (i in colors.indices) colors[i] = fromColors[i] + (toColors[i] - fromColors[i]) * eased
        val radius = max(width, height) * RADIUS
        for (i in 0 until GLOWS) {
            glowData[i * 4] = glows[i].x * width
            glowData[i * 4 + 1] = glows[i].y * height
            glowData[i * 4 + 2] = glows[i].size * radius
            glowData[i * 4 + 3] = (glows[i].strength * BackdropStrength).coerceAtMost(1f)
        }
        GLES20.glViewport(0, 0, width, height)
        GLES20.glUseProgram(program)
        GLES20.glUniform2f(uniform("size"), width.toFloat(), height.toFloat())
        GLES20.glUniform3fv(uniform("surface"), 1, SURFACE, 0)
        GLES20.glUniform4fv(uniform("glows"), GLOWS, glowData, 0)
        GLES20.glUniform3fv(uniform("colors"), GLOWS, colors, 0)
        GLES20.glUniform1f(uniform("fade"), eased)
        GLES20.glUniform1f(uniform("grain"), (frame++ % 64).toFloat())
        for (i in 0..1) {
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0 + i)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textures[if (i == 0) 1 - front else front])
        }
        GLES20.glUniform1i(uniform("before"), 0)
        GLES20.glUniform1i(uniform("card"), 1)
        val position = GLES20.glGetAttribLocation(program, "position")
        GLES20.glEnableVertexAttribArray(position)
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        EGLExt.eglPresentationTimeANDROID(display, window, us * 1000)
        check(EGL14.eglSwapBuffers(display, window)) { "Couldn't give the encoder a frame: ${EGL14.eglGetError()}" }
    }

    fun release() {
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
        if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window)
        // The program and textures go with the context. The display is the app's UI's too, so it stays.
        if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
        window = EGL14.EGL_NO_SURFACE
        context = EGL14.EGL_NO_CONTEXT
        EGL14.eglReleaseThread()
    }

    private val uniforms = HashMap<String, Int>()
    private fun uniform(name: String) = uniforms.getOrPut(name) { GLES20.glGetUniformLocation(program, name) }

    private fun upload(texture: Int, bitmap: Bitmap) {
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        // Android's bitmaps are premultiplied, and the shader blends them so.
        GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
    }

    private fun link(): Int {
        val program = GLES20.glCreateProgram()
        for ((type, source) in listOf(GLES20.GL_VERTEX_SHADER to VERTEX, GLES20.GL_FRAGMENT_SHADER to FRAGMENT)) {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0)
            check(ok[0] != 0) { "Shader: ${GLES20.glGetShaderInfoLog(shader)}" }
            GLES20.glAttachShader(program, shader)
        }
        GLES20.glLinkProgram(program)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, ok, 0)
        check(ok[0] != 0) { "Shader: ${GLES20.glGetProgramInfoLog(program)}" }
        return program
    }

    private companion object {
        const val GLOWS = 4
        // ArtBackdrop's, of the longer side.
        const val RADIUS = 0.75f
        const val FADE_S = 0.7f
        // The dark theme's surface, as the app has without dynamic colour.
        val SURFACE = floatArrayOf(0x14 / 255f, 0x12 / 255f, 0x18 / 255f)
        // EGLExt's constant is only there from Android 8.
        const val EGL_RECORDABLE_ANDROID = 0x3142

        const val VERTEX = """
            attribute vec2 position;
            varying vec2 at;
            void main() {
                at = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
                gl_Position = vec4(position, 0.0, 1.0);
            }
        """

        // As ArtBackdrop draws: each glow over the last, from its strength at the centre straight
        // down to nothing. A grain of about a step of colour, new each frame, keeps the encoder
        // from turning so soft a gradient into bands.
        const val FRAGMENT = """
            #ifdef GL_FRAGMENT_PRECISION_HIGH
            precision highp float;
            #else
            precision mediump float;
            #endif
            varying vec2 at;
            uniform vec2 size;
            uniform vec3 surface;
            uniform vec4 glows[4];
            uniform vec3 colors[4];
            uniform sampler2D before;
            uniform sampler2D card;
            uniform float fade;
            uniform float grain;

            float noise(vec2 p) {
                return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
            }

            void main() {
                vec2 p = at * size;
                vec3 c = surface;
                for (int i = 0; i < 4; i++) {
                    float a = clamp(1.0 - distance(p, glows[i].xy) / glows[i].z, 0.0, 1.0) * glows[i].w;
                    c = mix(c, colors[i], a);
                }
                c += (noise(p + grain * 5.588238) + noise(p + 47.0 + grain * 3.1) - 1.0) / 255.0;
                vec4 over = mix(texture2D(before, at), texture2D(card, at), fade);
                gl_FragColor = vec4(c * (1.0 - over.a) + over.rgb, 1.0);
            }
        """
    }
}
