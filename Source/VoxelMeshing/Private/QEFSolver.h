// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Quadratic Error Function solver for Dual Contouring.
 *
 * Minimizes: sum_i (n_i . (v - p_i))^2
 * where p_i are edge crossing points and n_i are surface normals.
 *
 * Uses SVD via Jacobi eigenvalue decomposition on the 3x3 normal matrix A^T*A.
 * Threshold controls which singular values are treated as zero (degenerate axes).
 */
struct FQEFSolver
{
	/** Accumulated A^T*A matrix (symmetric 3x3) */
	float ATA[3][3];

	/** Accumulated A^T*b vector */
	float ATb[3];

	/** Accumulated b^T*b scalar (for error computation) */
	float BTB;

	/** Mass point (average of all intersection points) */
	FVector3f MassPoint;

	/** Number of intersection planes added */
	int32 Count;

	FQEFSolver()
	{
		Reset();
	}

	void Reset()
	{
		FMemory::Memzero(ATA, sizeof(ATA));
		FMemory::Memzero(ATb, sizeof(ATb));
		BTB = 0.0f;
		MassPoint = FVector3f::ZeroVector;
		Count = 0;
	}

	/** Add an intersection plane defined by a point and normal. */
	void Add(const FVector3f& Point, const FVector3f& Normal)
	{
		// Accumulate A^T*A (outer product of normal)
		ATA[0][0] += Normal.X * Normal.X;
		ATA[0][1] += Normal.X * Normal.Y;
		ATA[0][2] += Normal.X * Normal.Z;
		ATA[1][1] += Normal.Y * Normal.Y;
		ATA[1][2] += Normal.Y * Normal.Z;
		ATA[2][2] += Normal.Z * Normal.Z;

		// Accumulate A^T*b where b_i = n_i . p_i
		const float Dot = FVector3f::DotProduct(Normal, Point);
		ATb[0] += Normal.X * Dot;
		ATb[1] += Normal.Y * Dot;
		ATb[2] += Normal.Z * Dot;

		BTB += Dot * Dot;

		// Accumulate mass point
		MassPoint += Point;
		Count++;
	}

	/** Merge another QEF's accumulated data into this one. */
	void Merge(const FQEFSolver& Other)
	{
		for (int32 i = 0; i < 3; i++)
		{
			for (int32 j = i; j < 3; j++)
			{
				ATA[i][j] += Other.ATA[i][j];
			}
			ATb[i] += Other.ATb[i];
		}
		BTB += Other.BTB;
		MassPoint += Other.MassPoint;
		Count += Other.Count;
	}

	/**
	 * Solve the QEF and return the optimal vertex position.
	 *
	 * @param SVDThreshold Singular values below this are zeroed (controls feature sharpness)
	 * @param CellBounds Bounding box for the cell — result is blended toward mass point if outside
	 * @param BiasStrength How aggressively to blend toward mass point when outside bounds (0-1)
	 * @return Optimal vertex position
	 */
	FVector3f Solve(float SVDThreshold, const FBox3f& CellBounds, float BiasStrength) const
	{
		if (Count == 0)
		{
			return CellBounds.GetCenter();
		}

		const FVector3f MP = MassPoint / static_cast<float>(Count);

		if (Count == 1)
		{
			return MP;
		}

		// Fill symmetric part
		float M[3][3];
		M[0][0] = ATA[0][0]; M[0][1] = ATA[0][1]; M[0][2] = ATA[0][2];
		M[1][0] = ATA[0][1]; M[1][1] = ATA[1][1]; M[1][2] = ATA[1][2];
		M[2][0] = ATA[0][2]; M[2][1] = ATA[1][2]; M[2][2] = ATA[2][2];

		// Solve (A^T*A) * v = A^T*b using SVD of the symmetric matrix
		// Jacobi eigenvalue decomposition: find eigenvalues and eigenvectors of A^T*A
		float Eigenvalues[3];
		float V[3][3]; // Eigenvectors as columns

		JacobiEigen3x3(M, Eigenvalues, V);

		// Pseudoinverse solution: v = V * S^-1 * V^T * (ATb)
		// where S^-1 inverts only eigenvalues above threshold
		float Result[3] = {0.0f, 0.0f, 0.0f};

		for (int32 i = 0; i < 3; i++)
		{
			if (Eigenvalues[i] > SVDThreshold)
			{
				// Project ATb onto eigenvector i
				float Proj = 0.0f;
				for (int32 j = 0; j < 3; j++)
				{
					Proj += V[j][i] * ATb[j];
				}
				Proj /= Eigenvalues[i];

				// Accumulate into result
				for (int32 j = 0; j < 3; j++)
				{
					Result[j] += V[j][i] * Proj;
				}
			}
		}

		FVector3f QEFResult(Result[0], Result[1], Result[2]);

		// Clamp/blend toward mass point if outside cell bounds
		if (!CellBounds.IsInside(QEFResult))
		{
			const FVector3f Closest = CellBounds.GetClosestPointTo(QEFResult);
			const float DistOutside = FVector3f::Distance(QEFResult, Closest);
			const float CellSize = CellBounds.GetExtent().X * 2.0f;
			const float Blend = FMath::Clamp(DistOutside / FMath::Max(CellSize, 0.001f) * BiasStrength * 2.0f, 0.0f, 1.0f);
			QEFResult = FMath::Lerp(QEFResult, MP, Blend);
		}

		return QEFResult;
	}

private:
	/**
	 * Jacobi eigenvalue decomposition for a 3x3 symmetric matrix.
	 * Finds eigenvalues and eigenvectors via iterative Givens rotations.
	 */
	static void JacobiEigen3x3(float M[3][3], float OutEigenvalues[3], float OutEigenvectors[3][3])
	{
		// Initialize eigenvectors to identity
		for (int32 i = 0; i < 3; i++)
		{
			for (int32 j = 0; j < 3; j++)
			{
				OutEigenvectors[i][j] = (i == j) ? 1.0f : 0.0f;
			}
		}

		// Copy matrix (we modify it in place)
		float A[3][3];
		FMemory::Memcpy(A, M, sizeof(A));

		// Jacobi iteration
		constexpr int32 MaxIterations = 20;
		for (int32 Iter = 0; Iter < MaxIterations; Iter++)
		{
			// Find largest off-diagonal element
			int32 p = 0, q = 1;
			float MaxVal = FMath::Abs(A[0][1]);

			if (FMath::Abs(A[0][2]) > MaxVal) { p = 0; q = 2; MaxVal = FMath::Abs(A[0][2]); }
			if (FMath::Abs(A[1][2]) > MaxVal) { p = 1; q = 2; MaxVal = FMath::Abs(A[1][2]); }

			// Convergence check
			if (MaxVal < 1e-8f)
			{
				break;
			}

			// Compute Givens rotation
			const float Diff = A[q][q] - A[p][p];
			float t;
			if (FMath::Abs(Diff) < 1e-10f)
			{
				t = 1.0f;
			}
			else
			{
				const float Phi = Diff / (2.0f * A[p][q]);
				t = 1.0f / (FMath::Abs(Phi) + FMath::Sqrt(Phi * Phi + 1.0f));
				if (Phi < 0.0f) t = -t;
			}

			const float c = 1.0f / FMath::Sqrt(t * t + 1.0f);
			const float s = t * c;

			// Apply rotation to A
			const float Tau = s / (1.0f + c);
			const float Apq = A[p][q];
			A[p][q] = 0.0f;
			A[p][p] -= t * Apq;
			A[q][q] += t * Apq;

			// Update remaining elements
			for (int32 r = 0; r < 3; r++)
			{
				if (r != p && r != q)
				{
					const float Arp = A[r][p];
					const float Arq = A[r][q];
					A[r][p] = A[p][r] = Arp - s * (Arq + Tau * Arp);
					A[r][q] = A[q][r] = Arq + s * (Arp - Tau * Arq);
				}
			}

			// Update eigenvectors
			for (int32 r = 0; r < 3; r++)
			{
				const float Vp = OutEigenvectors[r][p];
				const float Vq = OutEigenvectors[r][q];
				OutEigenvectors[r][p] = Vp - s * (Vq + Tau * Vp);
				OutEigenvectors[r][q] = Vq + s * (Vp - Tau * Vq);
			}
		}

		// Extract eigenvalues from diagonal
		OutEigenvalues[0] = A[0][0];
		OutEigenvalues[1] = A[1][1];
		OutEigenvalues[2] = A[2][2];
	}
};
