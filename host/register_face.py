#!/usr/bin/env python3
"""
Register a face for identity verification with find_face.py.

Takes one or more photos, extracts face embeddings using InsightFace ArcFace,
averages them, and saves to a .pkl file.

Usage:
  python register_face.py photo1.jpg photo2.jpg -o my_face.pkl
  python register_face.py photos/*.jpg -o my_face.pkl
"""
import argparse
import pickle
import sys
import numpy as np


def main():
    parser = argparse.ArgumentParser(description="Register face embedding for StackChan")
    parser.add_argument("photos", nargs="+", help="Photo files containing your face")
    parser.add_argument("-o", "--output", default="face.pkl", help="Output .pkl file (default: face.pkl)")
    args = parser.parse_args()

    print("Loading InsightFace model (first run downloads ~300MB)...")
    from insightface.app import FaceAnalysis
    app = FaceAnalysis(name="buffalo_l", providers=["CPUExecutionProvider"])
    app.prepare(ctx_id=0, det_size=(640, 640))

    import cv2
    embeddings = []

    for path in args.photos:
        print(f"  Processing {path}...", end=" ")
        img = cv2.imread(path)
        if img is None:
            print("SKIP (cannot read)")
            continue
        faces = app.get(img)
        if not faces:
            print("SKIP (no face detected)")
            continue
        face = max(faces, key=lambda f: (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]))
        embeddings.append(face.normed_embedding)
        print(f"OK (confidence: {face.det_score:.2f})")

    if not embeddings:
        print("\nNo faces found in any photo. Please provide clear face photos.")
        sys.exit(1)

    avg_embedding = np.mean(embeddings, axis=0)
    avg_embedding = avg_embedding / np.linalg.norm(avg_embedding)

    with open(args.output, "wb") as f:
        pickle.dump({"embedding": avg_embedding}, f)

    print(f"\nSaved face embedding to {args.output} ({len(embeddings)} photos averaged)")
    print(f"Use with: python find_face.py --face-db {args.output}")


if __name__ == "__main__":
    main()
